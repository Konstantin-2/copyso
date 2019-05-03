#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <set>
#include <filesystem>
#include <cstring>
#include <cassert>
#include <cerrno>
#include <ext/stdio_filebuf.h>
#include <unistd.h>
#include <fcntl.h>
#include <locale.h>
#include <libintl.h>
#include <getopt.h>
#include <glob.h>
#include <error.h>
#define _(STRING) gettext(STRING)
#define DATAROOTDIR "/usr/local/share"

using namespace std;
namespace fs = std::filesystem;

fs::path srcdir; // Source root directory
fs::path dstdir; // Destination root directory
vector<string> so_path; // Directories relative to srcdir with so files
set<string> list_so; // copied so-files (filenames only)
bool try_link = false; // Should program try to make hard link instead of copy files
bool verbose = false;

static void copy_absurl(fs::path src, fs::path dst);
static void copy_item(fs::path rel);

static constexpr std::string_view operator "" _s (const char* str, const size_t size)
{
	return std::string_view(str, size);
}

static void show_version()
{
	cout << _("copyso 0.1\nCopyright (C) 2019 Oshepkov Kosntantin\n"
	"License GPLv3+: GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>\n"
	"This is free software: you are free to change and redistribute it.\n"
	"There is NO WARRANTY, to the extent permitted by law.\n");
	exit(0);
}

static void show_help()
{
	cout << _("Usage: copyso [OPTION] <source> ... <dest>\n"
			"Copy executable files and its' dependencies to <dest> directory\n"
			"The program is useful when creating Live CD\n\n"
			"Options:\n"
			"  -r, --root=FROM  root directory to search files\n"
			"  -l, --link       try to make hard links instead of copy files\n"
			"  -v, --verbose    explain what is being done\n"
			"      --help       display this help and exit\n"
			"      --version    output version information and exit\n"
			"Report bugs to: oks-mgn@mail.ru\n"
			"copyko home page: <NOT YET, TODO>\n"
			"General help using GNU software: <https://www.gnu.org/gethelp/>\n");
	exit(0);
}

/* Returns true if substr is in begin of str.
 * Also shifts begin of str to length of substr
 * Return false and does not change str othervise.
 * Ex.: inbegin_shift("verylongline", "very") => true, str => "longline" */
static bool inbegin_shift(string_view& str, string_view substr)
{
	if (str.size() < substr.size()) return false;
	if (memcmp(str.data(), substr.data(), substr.size())) return false;
	str.remove_prefix(substr.size());
	return true;
}

/* Read file "filename" with format like ld.so.conf,
 * add directories specified there to "res",
 * recursively process "include" directive
 * reqlev - recursion level */
static void get_src_content(set<string>& res, const string& filename, int reqlev = 0)
{
	if (reqlev > 64) {
		cerr << _("Deep recursion in ld.so.conf") << '\n';
		return;
	}
	ifstream f(filename);
	if (!f) {
		cerr << _("Can't open file ") << filename << '\n';
		return;
	}
	string line;
	while(getline(f, line)) {
		if(line.empty() || line.front() == '#') continue;
		size_t i1 = 0;
		while (i1 < line.size() && isspace(line[i1])) i1++;
		size_t i2 = i1;
		while (i2 < line.size() && line[i2] != '#') i2++;
		while (i2 > i1 && isspace(line[i2 - 1])) i2--;
		string_view sub(line.data() + i1, i2 - i1);
		if (sub[0] == '/') {
			while (!sub.empty() && sub[0] == '/') sub.remove_prefix(1);
			while (!sub.empty() && sub.back() == '/') sub.remove_suffix(1);
			if (fs::is_directory(srcdir / sub))
				res.emplace(sub);
			continue;
		}
		if (!inbegin_shift(sub, "include"_s)) continue;
		if (sub.empty() || !isspace(sub[0])) continue;
		sub.remove_prefix(1);
		while(!sub.empty() && isspace(sub[0])) sub.remove_prefix(1);
		if (sub.empty()) continue;

		// glob is used in kmod source
		glob_t glo;
		string npath = srcdir / sub;
		glob(npath.c_str(), GLOB_NOSORT, 0, &glo);
		for(size_t i = 0; i < glo.gl_pathc; i++)
			get_src_content(res, glo.gl_pathv[i], reqlev + 1);
		globfree(&glo);
	}
}

static void get_so_directories()
{
	set<string> tmp;
	get_src_content(tmp, srcdir / "etc/ld.so.conf");
	so_path.reserve(tmp.size());
	while (!tmp.empty())
		so_path.emplace_back(move(tmp.extract(tmp.begin()).value()));
}

static vector<string_view> parse_args(int argc, char ** argv)
{
	vector<string_view> res;
	int c;
	static const struct option longOpts[] = {
		{"root", required_argument, 0, 'r'},
		{"verbose", no_argument, 0, 'v'},
		{"link", no_argument, 0, 'l'},
		{"help", no_argument, 0, 0},
		{"version", no_argument, 0, 0},
		{0, no_argument, 0, 0}
	};
	int longIndex = 0;

	while ((c = getopt_long(argc, argv, "-lvr:", longOpts, &longIndex)) != -1) {
		switch (c) {
		case 'l':
			try_link = true;
			break;
		case 'v':
			verbose = true;
			break;
		case 'r':
			srcdir = optarg;
			break;
		case 0:
			if (!strcmp(optarg, "--help"))
				show_help();
			else if (!strcmp(optarg, "--version"))
				show_version();
			else
				error(1, 0, _("Unrecognized option %s"), optarg);
		default:
			res.emplace_back(optarg);
		}
	}
	if (res.size() < 2) {
		cout << _("Files to copy are not specified") << '\n';
		exit(0);
	}
	dstdir = res.back();
	res.pop_back();
	return res;
}

// Return relative path (relative to srcdir)
string find_so(string_view name)
{
	for (const string& s : so_path)
		if (fs::exists(srcdir / s / name)) {
			string res(s);
			res += '/';
			res += name;
			return s;
		}
	return string();
}

static bool is_executable(const fs::path& abs_path)
{
	ifstream f(abs_path);
	unsigned buf;
	f.read((char *)&buf, 4);
	return buf == 0x464C457F;
}

// Copy file content. Also create destination directory. Try to make hard link if global flag set.
static void copy_regular_file(const fs::path& abs_src, const fs::path& abs_dst)
{
	error_code ec;
	if (verbose)
		cout << abs_src << " => "_s << abs_dst << '\n';

	fs::create_directories(abs_dst.parent_path(), ec);
	if (ec && ec != errc::file_exists) {
		cerr << _("Can't create directory ") << abs_dst.parent_path() << ": " << strerror(errno) << '\n';
	}

	if (try_link) {
		static bool errflag = false;
		fs::create_hard_link(abs_src, abs_dst, ec);
		if (ec && ec != errc::file_exists && !errflag) {
			cerr << _("Can't make hard link for file ") << abs_src << ": " << strerror(errno) << '\n';
			errflag = true;
		}
	}
	fs::copy_file(abs_src, abs_dst, ec);
	if (ec && ec != errc::file_exists)
			cerr << _("Can't copy ") << abs_src << " to " << abs_dst << ": " << strerror(errno) << '\n';
}

static void copy_bin_deps(fs::path abs_path)
{
	int pfd[2];
	if (pipe(pfd)) error(1, errno, "pipe error");
	pid_t pid = fork();
	if (pid == -1) error(1, errno, "fork error");

	if (!pid) {
		if (dup2(pfd[1], STDOUT_FILENO) == -1) error(1, errno, "dup2() error");
		close(pfd[1]);
		close(pfd[0]);

		char * args[4] = {(char *)"objdump", (char *)"-p", (char *)abs_path.c_str(), NULL};
		execvp(args[0], args);
		error(1, errno, _("Can't run %s"), args[0]);
	}
	close(pfd[1]);

	__gnu_cxx::stdio_filebuf<char> buf(pfd[0], std::ios::in);
	istream is(&buf);
	string line;
	vector<string> res;
	bool depends_found = false;
	while(getline(is, line)) {
		string_view so_name(line);
		if (!inbegin_shift(so_name, "  NEEDED ")) continue;
		while (!so_name.empty() && isspace(so_name.front())) so_name.remove_prefix(1);
		while (!so_name.empty() && isspace(so_name.back())) so_name.remove_suffix(1);
		if (so_name.empty()) continue;
		string so_name_str(so_name);
		if(!list_so.insert(so_name_str).second) continue;
		string so_fullname = find_so(so_name);
		if (!so_fullname.empty())
			copy_item(so_fullname);
		else
			cout << so_name << _(" not found. Please, append required directory to /etc/ld.so.conf") << '\n';
	}
}

static void copy_item(fs::path rel)
{
	error_code ec;
	fs::path src = srcdir / rel;
	while (fs::is_symlink(src)) {
		fs::path dst = dstdir / rel;
		if (verbose)
			cout << src << " => " << dst << '\n';

		fs::create_directories(dst.parent_path(), ec);
		if (ec && ec != errc::file_exists) {
			cerr << _("Can not create directory ") << dst.parent_path() << '\n';
			return;
		}
		fs::copy_symlink(src, dst, ec);
		if (ec && ec != errc::file_exists) {
			cerr << _("Can not create symbolic link ") << src << " =>" << dst << '\n';
			return;
		}

		string t = fs::read_symlink(src);
		if (t.empty())
			cerr << _("Read error ") << src << '\n';
		else if (t[0] == '/') {
			copy_absurl(t, dst);
			return;
		}
		src = src.parent_path() / t;
		rel = fs::relative(src, srcdir);
	}

	if (fs::is_regular_file(src)) {
		copy_regular_file(src, dstdir / rel);
		if (is_executable(src))
			copy_bin_deps(src);
	} else
		cout << src << _(" skipped, file type unknown") << '\n';
}

/* Copy if src is symlink to absolute path \
 * dst - absolute path too */
static void copy_absurl(fs::path src, fs::path dst)
{
	while (fs::is_symlink(src)) {
		string t = fs::read_symlink(src);
		if (t.empty())
			cerr << _("Error read ") << src << '\n';
		else if (t[0] == '/')
			src = t;
		else
			src = src.parent_path() / t;
	}

	if (fs::is_regular_file(src)) {
		copy_regular_file(src, dst);
		if (is_executable(src))
			copy_bin_deps(src);
	} else
		cout << src << _(" skipped, file type unknown") << '\n';
}

static void copy_dir(string_view dir)
{
	for(auto& p: fs::directory_iterator(dir))
		copy_item(p.path());
}

void copy_param(string_view item)
{
	if (item.empty()) return;
	if (fs::is_directory(item))
		copy_dir(item);
	else if (fs::is_regular_file(item) || fs::is_symlink(item))
		copy_item(item);
	else
		cout << item << _(" skipped, file type unknown") << '\n';
}

int main(int argc, char ** argv)
{
	assert(sizeof(unsigned) == 4);
	setlocale(LC_ALL, "");
	bindtextdomain("copyelf", DATAROOTDIR "/locale");
	textdomain("copyelf");
	vector<string_view> args = parse_args(argc, argv);
	get_so_directories();
	for (string_view& i : args)
		copy_param(i);
}
