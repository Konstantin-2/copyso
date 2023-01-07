#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <filesystem>
#include <cstring>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <ext/stdio_filebuf.h>
#include <unistd.h>
#include <fcntl.h>
#include <locale.h>
#include <libintl.h>
#include <getopt.h>
#include <glob.h>
#include <error.h>
#include <sys/wait.h>
#define _(STRING) gettext(STRING)

using namespace std;
namespace fs = std::filesystem;

fs::path srcdir; // Source root directory
fs::path dstdir; // Destination root directory
fs::path srclib; // Root directory to search ld.conf.so and so-files
fs::path dstlib; // If defined, all so-files will be copied into same one this directory
bool has_dstlib = false;
vector<string> so_path; // Directories relative to srcdir with so files
set<string> list_so; // copied so-files (filenames only)
bool try_link = false; // Should program try to make hard link instead of copy files
bool verbose = false;
vector<string_view> paths; // Path to search files (see environment PATH)
enum class Arch {x86, x64, unknown};
Arch need_arch = Arch::unknown;

static void copy_absurl(fs::path src, fs::path dst);
static void copy_item(fs::path rel, bool = false);
static Arch get_arch(const string& fn);

static constexpr std::string_view operator "" _s (const char* str, const size_t size)
{
	return std::string_view(str, size);
}

static void show_version()
{
	cout << _("copyso 0.3\nCopyright (C) 2019-2022 Oshepkov Kosntantin\n"
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
			"  -r, --root=FROM   root directory to search files\n"
			"      --srclib=FROM root directory to search so-files\n"
			"      --dstlib=TO   subdirectory in <dest> to store so-files\n"
			"  -l, --link        try to make hard links instead of copy files\n"
			"  -p, --path        use environment PATH to search files in root\n"
			"  -v, --verbose     explain what is being done\n"
			"      --x86         copy 32-bit libs\n"
			"      --x64         copy 64-bit libs\n"
			"      --help        display this help and exit\n"
			"      --version     output version information and exit\n"
			"Report bugs to: oks-mgn@mail.ru\n"
			"copyso home page: https://github.com/Konstantin-2/copyso.git\n"
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

// As std::filesystem::relative, but preserves symlinks, does not resolve them
static string alt_relative(const fs::path& path, const fs::path& dir)
{
	string path_s = path;
	string dir_s = dir;
	string_view res = path_s;
	if (!inbegin_shift(res, dir_s))
		return string();
	while(!res.empty() && res[0] == '/')
		res.remove_prefix(1);
	return string(res);
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
			while (!sub.empty() && sub.back() == '/') sub.remove_suffix(1);
			if (fs::is_directory(srclib / sub))
				res.emplace(sub);
			continue;
		}
		if (!inbegin_shift(sub, "include"_s)) continue;
		if (sub.empty() || !isspace(sub[0])) continue;
		sub.remove_prefix(1);
		while(!sub.empty() && isspace(sub[0])) sub.remove_prefix(1);
		if (sub.empty()) continue;

		// glob is used in kmod source
		string npath;
		if (sub[0] == '/')
			npath = srclib / sub;
		else
			npath = srclib / "etc/" / sub;
		glob_t glo;
		glob(npath.c_str(), GLOB_NOSORT, 0, &glo);
		for(size_t i = 0; i < glo.gl_pathc; i++)
			get_src_content(res, glo.gl_pathv[i], reqlev + 1);
		globfree(&glo);
	}
}

static void get_so_directories()
{
	set<string> tmp;
	get_src_content(tmp, srclib / "etc/ld.so.conf");
	tmp.insert("/lib");
	tmp.insert("/lib64");
	tmp.insert("/usr/lib");
	tmp.insert("/usr/lib64");
	so_path.reserve(tmp.size());
	while (!tmp.empty())
		so_path.emplace_back(move(tmp.extract(tmp.begin()).value()));

	if (verbose) {
		cout << _("Directories with so-files:") << '\n';
		for (const string& s : so_path)
			cout << s << '\n';
	}
}

// Split string (PATH) by columns: "/bin:/sbin:..." => {"/bin", "/sbin", ...}
static vector<string_view> split_path(string_view strv)
{
	vector<string_view> output;
	size_t first = 0;
	while (first < strv.size())
	{
		auto second = strv.find_first_of(':', first);
		if (second == string_view::npos)
			second = strv.size();
		while (first < second && strv[first] == '/')
			first++;
		if (first < second)
			output.emplace_back(strv.substr(first, second-first));
		first = second + 1;
	}
	return output;
}

static vector<string_view> parse_args(int argc, char ** argv)
{
	vector<string_view> res;
	int c;
	char * srcdir_s = 0;
	char * srclib_s = 0;
	char * dstlib_s = 0;
	bool use_path = false;
	static const struct option longOpts[] = {
		{"root", required_argument, 0, 'r'},
		{"verbose", no_argument, 0, 'v'},
		{"link", no_argument, 0, 'l'},
		{"path", no_argument, 0, 'p'},
		{"help", no_argument, 0, 2},
		{"version", no_argument, 0, 3},
		{"srclib", required_argument, 0, 4},
		{"dstlib", required_argument, 0, 5},
		{"x86", no_argument, 0, 6},
		{"x64", no_argument, 0, 7},
		{0, no_argument, 0, 0}
	};
	int longIndex = 0;

	while ((c = getopt_long(argc, argv, "-plvr:", longOpts, &longIndex)) != -1) {
		switch (c) {
		case 'l':
			try_link = true;
			break;
		case 'v':
			verbose = true;
			break;
		case 'p':
			use_path = true;
			break;
		case 'r':
			srcdir_s = optarg;
			break;
		case 2:
			show_help();
			break;
		case 3:
			show_version();
			break;
		case 4:
			srclib_s = optarg;
			break;
		case 5:
			dstlib_s = optarg;
			break;
		case 6:
			need_arch = Arch::x86;
			break;
		case 7:
			need_arch = Arch::x64;
			break;
		default:
			if (optarg)
				res.emplace_back(optarg);
		}
	}
	if (res.empty()) {
		cout << _("Files to copy are not specified") << '\n';
		exit(0);
	} else if (res.size() == 1) {
		cout << _("Destination directory is not specified") << '\n';
		exit(0);
	}
	srcdir = srcdir_s ? srcdir_s : "/";
	dstdir = res.back();
	srclib = srclib_s ? srclib_s : srcdir;
	if (dstlib_s) dstlib = dstlib_s, has_dstlib = true;
	res.pop_back();
	if (verbose) {
		cout << _("Source directory: ") << srcdir << '\n'
			<< _("Destination directory: ") << dstdir << '\n'
			<< _("Files to copy:");
		for (const string_view sv: res)
			cout << ' '<< sv;
		cout << '\n';
	}

	if (use_path) {
		char * env = getenv("PATH");
		paths = split_path(env);
	}
	if (paths.empty())
		paths.emplace_back();
	return res;
}

fs::path mksrc(fs::path rel, bool so)
{
	return so ? srclib / rel : srcdir / rel;
}

fs::path mkdst(fs::path rel, bool so)
{
	return so && has_dstlib ? dstdir / dstlib / rel.filename() : dstdir / rel;
}

// Return relative path (relative to srcdir)
string find_so(string_view name)
{
	for (const string& s : so_path)
		if (fs::exists(srclib / s / name)) {
			string res(s);
			res += '/';
			res += name;
			while (!res.empty() && res[0] == '/')
				res.erase(0, 1);
			if (need_arch == Arch::unknown)
				return res;
			if (need_arch == get_arch(res))
				return res;
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
		cerr << _("Can not create directory ") << abs_dst.parent_path() << ": " << strerror(errno) << '\n';
	}

	if (try_link) {
		static bool errflag = false;
		fs::create_hard_link(abs_src, abs_dst, ec);
		if (ec && ec != errc::file_exists && !errflag) {
			cerr << _("Can not make hard link for file ") << abs_src << ": " << strerror(errno) << '\n';
			errflag = true;
		}
	}
	fs::copy_file(abs_src, abs_dst, ec);
	if (ec && ec != errc::file_exists)
			cerr << _("Can not copy ") << abs_src << " to " << abs_dst << ": " << strerror(errno) << '\n';
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
	while(getline(is, line)) {
		string_view so_name(line);
		if (!inbegin_shift(so_name, "  NEEDED ")) continue;
		while (!so_name.empty() && isspace(so_name.front())) so_name.remove_prefix(1);
		while (!so_name.empty() && isspace(so_name.back())) so_name.remove_suffix(1);
		if (so_name.empty()) continue;
		string so_name_str(so_name);
		if(!list_so.insert(so_name_str).second) {
			if (verbose)
				cout << so_name_str << _(" already copied") << '\n';
			continue;
		}
		string so_fullname = find_so(so_name);
		if (!so_fullname.empty())
			copy_item(so_fullname, true);
		else
			cout << so_name << _(" not found. Please, append required directory to /etc/ld.so.conf") << '\n';
	}
	int status;
	waitpid(pid, &status, 0);
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

static void copy_item(fs::path rel, bool so)
{
	error_code ec;
	if (verbose)
		cout << _("Copy ") << rel << '\n';
	fs::path src = mksrc(rel, so);
	while (fs::is_symlink(src)) {
		fs::path dst = mkdst(rel, so);
		if (verbose)
			cout << src << " => " << dst << '\n';

		fs::create_directories(dst.parent_path(), ec);
		if (ec && ec != errc::file_exists) {
			cerr << _("Can not create directory ") << dst.parent_path() << '\n';
			return;
		}

		string t = fs::read_symlink(src);
		if (t.empty()) {
			cerr << _("Read error ") << src << '\n';
			return;
		} else if (t[0] == '/') {
			copy_absurl(t, dst);
			return;
		}
		fs::copy_symlink(src, dst, ec);
		if (ec && ec != errc::file_exists) {
			cerr << _("Can not create symbolic link ") << src << " =>" << dst << '\n';
			return;
		}


		src = src.parent_path() / t;
		string s = alt_relative(src, so ? srclib : srcdir);
		if (s.empty()) {
			copy_absurl(src, dst);
			return;
		}
		rel = s;
	}

	if (fs::is_regular_file(src)) {
		copy_regular_file(src, mkdst(rel, so));
		if (is_executable(src))
			copy_bin_deps(src);
	} else if (fs::is_directory(src))
		cout << _("Skipped directory ") << src << '\n';
	else
		cout << src << _(" skipped, file type unknown") << '\n';
}

static void copy_dir(fs::path src)
{
	for(auto& p: fs::directory_iterator(src)) {
		fs::path path = alt_relative(p, srcdir);
		if (path.empty()) {
			cerr << _("Error processing file while copying directory: ") << p << endl;
			continue;
		}
		copy_item(path);
	}
}

void copy_param(string_view item)
{
	while (!item.empty() && item[0] == '/')
		item.remove_prefix(1);
	if (item.empty()) return;
	fs::path src = srcdir / item;
	if (fs::is_directory(src)) {
		copy_dir(src);
		return;
	}
	for (const string_view& p : paths) {
		fs::path src = srcdir / p / item;
		if (fs::is_regular_file(src) || fs::is_symlink(src)) {
			copy_item(fs::path(p) / item);
			return;
		}
	}
	cout << src << _(" skipped, file not found") << '\n';
}

Arch get_arch(const string& fn)
{
	int pfd[2];
	if (pipe(pfd)) error(1, errno, "pipe error");
	pid_t pid = fork();
	if (pid == -1) error(1, errno, "fork error");

	if (!pid) {
		if (dup2(pfd[1], STDOUT_FILENO) == -1) error(1, errno, "dup2() error");
		close(pfd[1]);
		close(pfd[0]);
		ostringstream ofn;
		ofn << '/' << fn;
		string s = ofn.str();

		char * args[4] = {(char *)"objdump", (char *)"-a", (char *)s.c_str(), NULL};
		execvp(args[0], args);
		error(1, errno, _("Can't run %s"), args[0]);
	}
	close(pfd[1]);

	__gnu_cxx::stdio_filebuf<char> buf(pfd[0], std::ios::in);
	istream is(&buf);
	string line;
	Arch res = Arch::unknown;
	while(getline(is, line)) {
		if (line.find("elf32-i386") != string::npos)
			res = Arch::x86;
		if (line.find("elf64-x86-64") != string::npos)
			res = Arch::x64;
	}
	if (verbose) {
		cout << "Arch " << fn;
		switch(res) {
		case Arch::x64: cout << " x64" << '\n'; break;
		case Arch::x86: cout << " x86" << '\n'; break;
		default: cout << " unknown" << '\n'; break;
		};
	}
	int status;
	waitpid(pid, &status, 0);
	return res;
}

int main(int argc, char ** argv)
{
	assert(sizeof(unsigned) == 4);
	setlocale(LC_ALL, "");
	bindtextdomain("copyso", DATAROOTDIR "/locale");
	textdomain("copyso");
	vector<string_view> args = parse_args(argc, argv);
	get_so_directories();
	for (string_view& i : args)
		copy_param(i);
}
