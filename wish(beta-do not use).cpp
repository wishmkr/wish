// ============================================================
// WishOS System Manager v2.0.0
// The central management tool for WishOS
// Replaces: init system + package manager + dependency resolver
//           + peer server + layer manager + history tracker (transactions)
//           + improved package format + real layer isolation
// NOTE: ServiceDef / Generation are data models only for now -- no
//       service-manager or generation-rollback CLI command exists yet.
// ============================================================

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <stack>
#include <queue>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <chrono>
#include <iomanip>
#include <thread>
#include <mutex>
#include <memory>
#include <functional>
#include <atomic>
#include <utility>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <ctime>

// POSIX headers
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <linux/unistd.h>
#include <sched.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <pwd.h>
#include <poll.h>
#include <termios.h>
#include <cstdint>
#include <sys/signalfd.h>
#include <sys/file.h>

// External libraries
#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <archive.h>
#include <archive_entry.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ============================================================
// Constants & Configuration
// ============================================================

namespace Config {
    constexpr int PEER_PORT = 44449;
    constexpr const char* VERSION = "2.0.0";
    constexpr const char* DEFAULT_REPO = "https://cdn.wishpkgs.org";
    constexpr const char* DEFAULT_ARCH = "aarch64";
    constexpr const char* SERVICES_DIR = "/etc/wish/services";
    constexpr const char* GENERATIONS_DIR = "/var/lib/wish/generations";
    
    inline std::string get_cache_dir() {
        const char* env = std::getenv("WISH_CACHE_DIR");
        return env ? env : "/var/cache/wish";
    }
    
    inline std::string get_lib_dir() {
        const char* env = std::getenv("WISH_LIB_DIR");
        return env ? env : "/var/lib/wish";
    }

    // Root of the managed system (where packages actually land, e.g. usr/bin/,
    // etc/...). Defaults to "/" like a normal package manager; overridable via
    // WISH_ROOT so installs can be sandboxed for testing without real root.
    inline std::string get_root_dir() {
        const char* env = std::getenv("WISH_ROOT");
        return env ? env : "/";
    }

    inline std::string get_services_dir() {
        const char* env = std::getenv("WISH_SERVICES_DIR");
        return env ? env : "/etc/wish/services";
    }

    inline std::string get_run_dir() {
        const char* env = std::getenv("WISH_RUN_DIR");
        return env ? env : "/run/wish";
    }

    inline std::string get_generations_dir() {
        return get_lib_dir() + "/generations";
    }

    // Directory on the host PATH where cross-layer executable wrappers land, so
    // typing `pacman` transparently runs it inside its layer.
    inline std::string get_wrappers_dir() {
        const char* env = std::getenv("WISH_WRAPPERS_DIR");
        return env ? env : "/usr/local/bin";
    }

    // Config subtrees (relative to the install root) captured into each
    // generation so a rollback can restore edited/deleted config, not just
    // package-owned files. Override with WISH_MANAGED_PATHS (colon-separated).
    inline std::vector<std::string> get_managed_config_paths() {
        const char* env = std::getenv("WISH_MANAGED_PATHS");
        std::vector<std::string> out;
        if (env && *env) {
            std::istringstream ss(env);
            std::string item;
            while (std::getline(ss, item, ':')) if (!item.empty()) out.push_back(item);
        } else {
            out.push_back("etc");
        }
        return out;
    }
    
    inline std::string get_repo_url() {
        const char* env = std::getenv("WISH_REPO_URL");
        return env ? env : DEFAULT_REPO;
    }
    
    inline std::string get_arch() {
        #ifdef __aarch64__
            return "aarch64";
        #elif defined(__x86_64__)
            return "x86_64";
        #elif defined(__arm__)
            return "armv7h";
        #else
            return DEFAULT_ARCH;
        #endif
    }
}

// ============================================================
// Utility: Result Type for Error Handling
// ============================================================

template<typename T>
class Result {
    bool ok_;
    T value_;
    std::string error_;
    
    // Tag dispatch: T=std::string olduğunda Result(T) ile Result(const std::string&)
    // aynı imzaya düşüp ambiguous hale geliyordu (derleyici testiyle doğrulandı).
    // Ortak ctor'ları kaldırıp success()/failure() üzerinden tag ile ayrıştırıyoruz.
    struct SuccessTag {};
    struct FailureTag {};
    Result(SuccessTag, T val) : ok_(true), value_(std::move(val)), error_("") {}
    Result(FailureTag, std::string err) : ok_(false), value_(), error_(std::move(err)) {}
    
public:
    bool ok() const { return ok_; }
    bool is_err() const { return !ok_; }
    const T& value() const { return value_; }
    T& value() { return value_; }
    const std::string& error() const { return error_; }
    
    operator bool() const { return ok_; }
    
    static Result<T> success(T val) { return Result<T>(SuccessTag{}, std::move(val)); }
    static Result<T> failure(const std::string& err) { return Result<T>(FailureTag{}, err); }
    static Result<T> failure(const char* err) { return Result<T>(FailureTag{}, std::string(err)); }
};

template<>
class Result<void> {
    bool ok_;
    std::string error_;
    
public:
    Result() : ok_(true), error_("") {}
    Result(const std::string& err) : ok_(false), error_(err) {}
    
    bool ok() const { return ok_; }
    bool is_err() const { return !ok_; }
    const std::string& error() const { return error_; }
    operator bool() const { return ok_; }
    
    static Result<void> success() { return Result<void>(); }
    static Result<void> failure(const std::string& err) { return Result<void>(err); }
    static Result<void> failure(const char* err) { return Result<void>(std::string(err)); }
};

// ============================================================
// Utility: Path Sanitization (Enhanced)
// ============================================================

class PathValidator {
public:
    static bool is_safe(const std::string& path) {
        if (path.empty()) return false;
        if (path.find("..") != std::string::npos) return false;
        if (path.find("//") != std::string::npos) return false;
        if (path[0] == '/') return false;
        for (char c : path) {
            if (c == '\0' || c == '\n' || c == '\r' || c == '\t')
                return false;
        }
        return true;
    }
    
    static bool is_safe_package_name(const std::string& name) {
        if (name.empty() || name.length() > 128) return false;
        if (!std::regex_match(name, std::regex("^[a-z0-9][a-z0-9-]*$")))
            return false;
        return is_safe(name);
    }

    // Validate archive entry path to prevent traversal.
    // A raw substring/find() prefix test would let dest="/var/x/foo" match a
    // sibling "/var/x/foo_evil/..." (also starts with "/var/x/foo"), and a
    // naive "dest + '/' prefix" fix breaks when dest is "/" itself (root
    // already ends in the separator, so demanding one more '/' after it
    // rejects every legitimate entry). lexically_relative sidesteps both: it
    // yields a path starting with ".." exactly when `full` falls outside
    // `dest`, regardless of whether dest is "/" or some deeper directory.
    static bool is_safe_archive_path(const std::string& path, const std::string& dest) {
        fs::path full = fs::weakly_canonical(dest + "/" + path);
        fs::path dest_canon = fs::weakly_canonical(dest);
        fs::path rel = full.lexically_relative(dest_canon);
        if (rel.empty()) return false;
        return *rel.begin() != "..";
    }

    // Validate a single path component (no separators at all) received from an
    // untrusted source, e.g. a filename requested over the peer-server socket.
    static bool is_safe_filename(const std::string& name) {
        if (name.empty() || name.length() > 256) return false;
        if (name == "." || name == "..") return false;
        static const std::regex pattern("^[A-Za-z0-9][A-Za-z0-9._-]*$");
        return std::regex_match(name, pattern);
    }

    // Resolve `name` under `dir` and confirm the canonical result stays inside
    // `dir` (defense in depth alongside is_safe_filename's character allowlist).
    static bool resolve_within(const std::string& dir, const std::string& name, fs::path& out) {
        std::error_code ec;
        fs::path candidate = fs::path(dir) / name;
        fs::path dir_canon = fs::weakly_canonical(dir, ec);
        if (ec) return false;
        fs::path full_canon = fs::weakly_canonical(candidate, ec);
        if (ec) return false;
        fs::path rel = full_canon.lexically_relative(dir_canon);
        if (rel.empty()) return false;
        auto first = *rel.begin();
        if (first == ".." || first == ".") return false; // must be strictly inside dir
        out = full_canon;
        return true;
    }
};

// ============================================================
// Utility: Security & Permissions (Enhanced)
// ============================================================

class Security {
public:
    static bool is_root() {
        return getuid() == 0;
    }
    
    static Result<void> require_root(const std::string& operation) {
        if (!is_root()) {
            return Result<void>::failure(operation + " requires root privileges");
        }
        return Result<void>::success();
    }
    
    static Result<void> drop_privileges(uid_t uid, gid_t gid) {
        if (setgid(gid) != 0 || setuid(uid) != 0) {
            return Result<void>::failure("Failed to drop privileges: " + std::string(strerror(errno)));
        }
        return Result<void>::success();
    }
    
    static bool check_safe_permissions(const std::string& path, mode_t max_mode) {
        struct stat st;
        if (stat(path.c_str(), &st) != 0) return false;
        return (st.st_mode & 0777) <= max_mode;
    }
};

// ============================================================
// Utility: Atomic File I/O
// ============================================================

// Writes to a sibling ".tmp" file and renames it over the target. rename(2)
// is atomic on the same filesystem, so a crash or power loss mid-write can
// never leave installed.json/history.json/the index cache truncated or
// half-written -- readers always see either the old or the new content.
inline bool write_file_atomic(const std::string& path, const std::string& content) {
    std::error_code ec;
    fs::path parent = fs::path(path).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);

    std::string tmp_path = path + ".tmp";
    {
        std::ofstream f(tmp_path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << content;
        f.flush();
        if (!f) return false;
    }
    fs::rename(tmp_path, path, ec);
    if (ec) {
        fs::remove(tmp_path, ec);
        return false;
    }
    return true;
}

// ============================================================
// Utility: Time & Logging
// ============================================================

class Logger {
public:
    enum Level { INFO, OK, WARN, ERROR };
    
    static void log(Level level, const std::string& msg) {
        const char* prefix[] = {"[info]", "[ok]", "[warn]", "[error]"};
        std::cout << prefix[level] << " " << msg << "\n";
    }
    
    static void info(const std::string& msg) { log(INFO, msg); }
    static void ok(const std::string& msg) { log(OK, msg); }
    static void warn(const std::string& msg) { log(WARN, msg); }
    static void error(const std::string& msg) { log(ERROR, msg); }
    
    static std::string timestamp() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time), "%Y-%m-%dT%H:%M:%S");
        return ss.str();
    }
};

// ============================================================
// Utility: HTTP/CURL Helpers
// ============================================================

class HttpClient {
    static size_t write_string(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }
    
    static size_t write_file(void* contents, size_t size, size_t nmemb, FILE* stream) {
        return fwrite(contents, size, nmemb, stream);
    }
    
    // Occasional connect()-phase stalls (e.g. a dropped SYN on flaky links)
    // otherwise burn the full CURLOPT_TIMEOUT before failing. A short
    // connect timeout plus a couple of retries turns a one-off stall into a
    // sub-second hiccup instead of aborting the whole install.
    static constexpr int kMaxAttempts = 3;

public:
    static Result<std::string> get(const std::string& url) {
        std::string last_err;
        for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
            CURL* curl = curl_easy_init();
            if (!curl) return Result<std::string>::failure("CURL init failed");

            std::string response;
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "wish/2.0.0");

            CURLcode res = curl_easy_perform(curl);
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            curl_easy_cleanup(curl);

            if (res != CURLE_OK) {
                last_err = std::string("CURL: ") + curl_easy_strerror(res);
                if (attempt < kMaxAttempts) { std::this_thread::sleep_for(std::chrono::milliseconds(300)); continue; }
                return Result<std::string>::failure(last_err);
            }
            if (http_code != 200) {
                return Result<std::string>::failure("HTTP " + std::to_string(http_code));
            }
            return Result<std::string>::success(response);
        }
        return Result<std::string>::failure(last_err);
    }

    static Result<void> download(const std::string& url, const std::string& dest) {
        std::string last_err;
        for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
            CURL* curl = curl_easy_init();
            if (!curl) return Result<void>::failure("CURL init failed");

            FILE* fp = fopen(dest.c_str(), "wb");
            if (!fp) {
                curl_easy_cleanup(curl);
                return Result<void>::failure("Cannot open: " + dest);
            }

            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "wish/2.0.0");

            CURLcode res = curl_easy_perform(curl);
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            curl_easy_cleanup(curl);
            fclose(fp);

            if (res != CURLE_OK) {
                fs::remove(dest);
                last_err = std::string("CURL: ") + curl_easy_strerror(res);
                if (attempt < kMaxAttempts) { std::this_thread::sleep_for(std::chrono::milliseconds(300)); continue; }
                return Result<void>::failure(last_err);
            }
            if (http_code != 200) {
                fs::remove(dest);
                return Result<void>::failure("HTTP " + std::to_string(http_code));
            }
            return Result<void>::success();
        }
        return Result<void>::failure(last_err);
    }
};

// ============================================================
// Utility: Cryptography (SHA256 + Signature Verification)
// ============================================================

class Crypto {
public:
    static Result<std::string> sha256_file(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return Result<std::string>::failure("Cannot open: " + path);
        
        EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
        if (!mdctx) return Result<std::string>::failure("EVP init failed");
        
        if (EVP_DigestInit_ex(mdctx, EVP_sha256(), nullptr) != 1) {
            EVP_MD_CTX_free(mdctx);
            return Result<std::string>::failure("EVP_DigestInit failed");
        }
        
        char buffer[8192];
        while (file.good()) {
            file.read(buffer, sizeof(buffer));
            if (file.gcount() > 0) {
                EVP_DigestUpdate(mdctx, buffer, file.gcount());
            }
        }
        
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hash_len = 0;
        EVP_DigestFinal_ex(mdctx, hash, &hash_len);
        EVP_MD_CTX_free(mdctx);
        
        std::stringstream hex;
        for (unsigned int i = 0; i < hash_len; i++) {
            hex << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        }
        return Result<std::string>::success(hex.str());
    }
    
    static Result<bool> verify_file(const std::string& path, const std::string& expected) {
        auto result = sha256_file(path);
        if (!result) return Result<bool>::failure(result.error());
        
        std::string computed = result.value();
        std::string expected_lower = expected;
        std::transform(expected_lower.begin(), expected_lower.end(), expected_lower.begin(), ::tolower);
        
        if (computed != expected_lower) {
            Logger::error("Checksum mismatch!");
            std::cerr << "  Expected: " << expected_lower << "\n";
            std::cerr << "  Got:      " << computed << "\n";
            return Result<bool>::success(false);
        }
        Logger::ok("Checksum verified");
        return Result<bool>::success(true);
    }
    
    static Result<bool> verify_signature(const std::string& data_path, 
                                          const std::string& sig_path,
                                          const std::string& pubkey_path) {
        FILE* pub_key_file = fopen(pubkey_path.c_str(), "r");
        if (!pub_key_file) {
            return Result<bool>::failure("Cannot open public key: " + pubkey_path);
        }
        
        EVP_PKEY* pubkey = PEM_read_PUBKEY(pub_key_file, nullptr, nullptr, nullptr);
        fclose(pub_key_file);
        
        if (!pubkey) {
            return Result<bool>::failure("Failed to read public key");
        }
        
        std::ifstream sig_file(sig_path, std::ios::binary);
        if (!sig_file) {
            EVP_PKEY_free(pubkey);
            return Result<bool>::failure("Cannot open signature file");
        }
        std::string signature((std::istreambuf_iterator<char>(sig_file)),
                              std::istreambuf_iterator<char>());
        
        std::ifstream data_file(data_path, std::ios::binary);
        if (!data_file) {
            EVP_PKEY_free(pubkey);
            return Result<bool>::failure("Cannot open data file");
        }
        std::string data((std::istreambuf_iterator<char>(data_file)),
                         std::istreambuf_iterator<char>());
        
        EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
        if (!mdctx) {
            EVP_PKEY_free(pubkey);
            return Result<bool>::failure("Failed to create verification context");
        }
        
        if (EVP_DigestVerifyInit(mdctx, nullptr, EVP_sha256(), nullptr, pubkey) != 1) {
            EVP_MD_CTX_free(mdctx);
            EVP_PKEY_free(pubkey);
            return Result<bool>::failure("EVP_DigestVerifyInit failed");
        }
        
        if (EVP_DigestVerifyUpdate(mdctx, data.data(), data.size()) != 1) {
            EVP_MD_CTX_free(mdctx);
            EVP_PKEY_free(pubkey);
            return Result<bool>::failure("EVP_DigestVerifyUpdate failed");
        }
        
        int result = EVP_DigestVerifyFinal(mdctx, 
                                           (unsigned char*)signature.data(), 
                                           signature.size());
        
        EVP_MD_CTX_free(mdctx);
        EVP_PKEY_free(pubkey);
        
        if (result == 1) {
            Logger::ok("Signature verified");
            return Result<bool>::success(true);
        } else if (result == 0) {
            Logger::error("Signature verification failed: invalid signature");
            return Result<bool>::success(false);
        } else {
            return Result<bool>::failure("Signature verification error");
        }
    }
};

// ============================================================
// Utility: Archive Extraction (Enhanced with path traversal protection)
// ============================================================

class ArchiveExtractor {
public:
    // Returns the full paths of every non-directory entry actually written to
    // disk. Callers used to diff a recursive_directory_iterator over dest_dir
    // before/after extraction to figure this out -- that breaks (and is
    // dangerous) once dest_dir is "/", since it means walking the whole live
    // filesystem including /proc and /sys. libarchive already tells us exactly
    // what it wrote, so report that directly instead.
    static Result<std::vector<std::string>> extract(const std::string& pkg_file, const std::string& dest_dir) {
        struct archive* a = archive_read_new();
        struct archive* ext = archive_write_disk_new();

        if (!a || !ext) {
            if (a) archive_read_free(a);
            if (ext) archive_write_free(ext);
            return Result<std::vector<std::string>>::failure("Archive init failed");
        }

        archive_read_support_format_tar(a);
        archive_read_support_filter_gzip(a);
        archive_read_support_filter_xz(a);
        archive_read_support_filter_zstd(a);

        int flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                    ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS;
        archive_write_disk_set_options(ext, flags);
        archive_write_disk_set_standard_lookup(ext);

        if (archive_read_open_filename(a, pkg_file.c_str(), 10240) != ARCHIVE_OK) {
            archive_read_free(a);
            archive_write_free(ext);
            return Result<std::vector<std::string>>::failure("Cannot open archive");
        }

        std::vector<std::string> extracted;
        struct archive_entry* entry;
        int r;
        while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
            const char* current_file = archive_entry_pathname(entry);

            if (!current_file) {
                archive_read_free(a);
                archive_write_free(ext);
                return Result<std::vector<std::string>>::failure("Invalid archive entry");
            }

            if (!PathValidator::is_safe_archive_path(current_file, dest_dir)) {
                Logger::warn("Blocked path traversal attempt: " + std::string(current_file));
                archive_read_data_skip(a);
                continue;
            }

            std::string dest_norm = dest_dir;
            while (dest_norm.size() > 1 && dest_norm.back() == '/') dest_norm.pop_back();
            if (dest_norm == "/") dest_norm.clear(); // avoid "//usr/..." when dest is root
            std::string full_path = dest_norm + "/" + current_file;
            archive_entry_set_pathname(entry, full_path.c_str());

            mode_t mode = archive_entry_mode(entry);
            if (mode & (S_ISUID | S_ISGID)) {
                Logger::warn("Stripping setuid/setgid from: " + std::string(current_file));
                mode &= ~(S_ISUID | S_ISGID);
                archive_entry_set_mode(entry, mode);
            }

            if (archive_read_extract2(a, entry, ext) != ARCHIVE_OK) {
                archive_read_free(a);
                archive_write_free(ext);
                return Result<std::vector<std::string>>::failure("Extract failed: " + std::string(archive_error_string(ext)));
            }

            if (archive_entry_filetype(entry) != AE_IFDIR) {
                extracted.push_back(full_path);
            }
        }

        archive_read_free(a);
        archive_write_free(ext);

        if (r != ARCHIVE_EOF) {
            return Result<std::vector<std::string>>::failure("Archive read error");
        }
        return Result<std::vector<std::string>>::success(extracted);
    }
};

// ============================================================
// Utility: Archive Packing (create .tar.gz for bundles)
// ============================================================

// Creates gzip-compressed tar archives from on-disk paths and in-memory
// buffers. Used to build the .wsh bundles produced by `wish bundle` -- the
// reverse of ArchiveExtractor.
class ArchivePacker {
    static void add_file_entry(struct archive* a, const std::string& disk, const std::string& arcname) {
        struct stat st;
        if (lstat(disk.c_str(), &st) != 0) return;

        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, arcname.c_str());
        archive_entry_copy_stat(entry, &st);

        if (S_ISLNK(st.st_mode)) {
            char buf[4096];
            ssize_t n = readlink(disk.c_str(), buf, sizeof(buf) - 1);
            if (n >= 0) { buf[n] = '\0'; archive_entry_set_symlink(entry, buf); }
            archive_entry_set_size(entry, 0);
        }

        archive_write_header(a, entry);

        if (S_ISREG(st.st_mode)) {
            std::ifstream f(disk, std::ios::binary);
            char buf[65536];
            while (f) {
                f.read(buf, sizeof(buf));
                std::streamsize got = f.gcount();
                if (got > 0) archive_write_data(a, buf, (size_t)got);
            }
        }
        archive_entry_free(entry);
    }

    static void add_buffer_entry(struct archive* a, const std::string& arcname, const std::string& content) {
        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, arcname.c_str());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(entry, content.size());
        archive_entry_set_mtime(entry, time(nullptr), 0);
        archive_write_header(a, entry);
        if (!content.empty()) archive_write_data(a, content.data(), content.size());
        archive_entry_free(entry);
    }

public:
    // Recursively expand a disk path into (disk, arcname) pairs (dirs first).
    static void expand(const std::string& disk, const std::string& arcname,
                       std::vector<std::pair<std::string, std::string>>& out) {
        struct stat st;
        if (lstat(disk.c_str(), &st) != 0) return;
        out.push_back({disk, arcname});
        if (S_ISDIR(st.st_mode)) {
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(disk, ec)) {
                expand(e.path().string(), arcname + "/" + e.path().filename().string(), out);
            }
        }
    }

    static Result<void> create_gz(const std::string& out_file,
                                  const std::vector<std::pair<std::string, std::string>>& disk_entries,
                                  const std::vector<std::pair<std::string, std::string>>& buffers) {
        struct archive* a = archive_write_new();
        archive_write_add_filter_gzip(a);
        archive_write_set_format_pax_restricted(a);

        if (archive_write_open_filename(a, out_file.c_str()) != ARCHIVE_OK) {
            std::string e = archive_error_string(a) ? archive_error_string(a) : "open failed";
            archive_write_free(a);
            return Result<void>::failure("Cannot create archive: " + e);
        }

        for (const auto& [arc, content] : buffers) add_buffer_entry(a, arc, content);
        for (const auto& [disk, arc] : disk_entries) add_file_entry(a, disk, arc);

        archive_write_close(a);
        archive_write_free(a);
        return Result<void>::success();
    }
};

// ============================================================
// Data Models (Enhanced for v2)
// ============================================================

struct PackageMetadata {
    std::string name;
    std::string version;
    std::string arch;
    std::vector<std::string> depends;
    std::string license;
    std::string maintainer;
    std::string description;
    
    json to_json() const {
        json j;
        j["name"] = name;
        j["version"] = version;
        j["arch"] = arch;
        j["depends"] = depends;
        j["license"] = license;
        j["maintainer"] = maintainer;
        j["description"] = description;
        return j;
    }
    
    static PackageMetadata from_json(const json& j) {
        PackageMetadata m;
        m.name = j.value("name", "");
        m.version = j.value("version", "");
        m.arch = j.value("arch", "");
        m.license = j.value("license", "");
        m.maintainer = j.value("maintainer", "");
        m.description = j.value("description", "");
        if (j.contains("depends")) {
            for (const auto& d : j["depends"]) m.depends.push_back(d);
        }
        return m;
    }
};

// ============================================================
// Dependency specifications: version constraints, OR-alternatives,
// virtual packages (provides), conflicts/breaks
// ============================================================
//
// Textual form shared between .info files (written by the mirror scripts,
// see tools/mirror_common.py's parse_dep_field/format_dep_field, which
// this must stay wire-compatible with) and installed.json:
//   "name1 (>=1.2.3)|name2,name3 (=2.0)"
// comma separates independent dependency slots (DepSpec), "|" separates
// alternatives within one slot (DepAlt) -- e.g. "A depends on (B>=1.2 OR C)
// AND D".

// Dotted-numeric version comparison only -- sufficient since
// mirror_common.sanitize_version() already strips every upstream version
// down to a bare [0-9]+(\.[0-9]+)* core (no epoch, no distro revision
// suffix) before it ever reaches wish. Returns -1/0/1 for a<b/a==b/a>b.
inline int compare_versions(const std::string& a, const std::string& b) {
    auto split = [](const std::string& v) {
        std::vector<int> out;
        std::stringstream ss(v);
        std::string part;
        while (std::getline(ss, part, '.')) {
            try { out.push_back(std::stoi(part)); } catch (...) { out.push_back(0); }
        }
        return out;
    };
    std::vector<int> pa = split(a), pb = split(b);
    size_t n = std::max(pa.size(), pb.size());
    for (size_t i = 0; i < n; i++) {
        int va = i < pa.size() ? pa[i] : 0;
        int vb = i < pb.size() ? pb[i] : 0;
        if (va != vb) return va < vb ? -1 : 1;
    }
    return 0;
}

struct VersionConstraint {
    std::string op;      // ">=","<=","=","<",">", or "" for "any version"
    std::string version;

    bool satisfied_by(const std::string& candidate_version) const {
        if (op.empty()) return true;
        int cmp = compare_versions(candidate_version, version);
        if (op == ">=") return cmp >= 0;
        if (op == "<=") return cmp <= 0;
        if (op == "=")  return cmp == 0;
        if (op == "<")  return cmp < 0;
        if (op == ">")  return cmp > 0;
        return true; // unrecognized operator -- don't block on something unparseable
    }

    std::string to_string() const { return op.empty() ? "" : (" (" + op + version + ")"); }
};

// One alternative within a dependency's OR-group ("A | B | C").
struct DepAlt {
    std::string name;
    VersionConstraint constraint;
    std::string to_string() const { return name + constraint.to_string(); }
};

// A single dependency slot: normally one name, but can hold several
// alternatives ("libgcc-s1 (>=3.0) | libgcc1") mirroring Debian's own
// alternation syntax -- the resolver tries each in turn.
struct DepSpec {
    std::vector<DepAlt> alternatives;

    const std::string& primary_name() const {
        static const std::string empty;
        return alternatives.empty() ? empty : alternatives[0].name;
    }

    static DepSpec parse(const std::string& raw) {
        DepSpec spec;
        std::stringstream ss(raw);
        std::string alt_str;
        while (std::getline(ss, alt_str, '|')) {
            size_t b = alt_str.find_first_not_of(" \t");
            if (b == std::string::npos) continue;
            size_t e = alt_str.find_last_not_of(" \t");
            alt_str = alt_str.substr(b, e - b + 1);

            DepAlt alt;
            size_t paren = alt_str.find('(');
            if (paren != std::string::npos) {
                std::string name_part = alt_str.substr(0, paren);
                size_t ne = name_part.find_last_not_of(" \t");
                alt.name = (ne == std::string::npos) ? "" : name_part.substr(0, ne + 1);

                size_t close = alt_str.find(')', paren);
                std::string inner = alt_str.substr(
                    paren + 1, (close == std::string::npos ? alt_str.size() : close) - paren - 1);
                static const char* ops[] = {">=", "<=", "=", "<", ">"};
                bool matched = false;
                for (const char* op : ops) {
                    size_t oplen = std::strlen(op);
                    if (inner.compare(0, oplen, op) == 0) {
                        alt.constraint.op = op;
                        alt.constraint.version = inner.substr(oplen);
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    // A "(...)" constraint block IS present but its
                    // operator isn't one we recognize -- silently treating
                    // this as "no constraint" would be worse than failing:
                    // it would let a version requirement nobody could
                    // actually verify quietly pass. Hard parse error
                    // instead, caught at the .info/installed.json loading
                    // boundary (RemoteIndex::get(), PackageDatabase::load())
                    // so one malformed entry doesn't take down the whole
                    // process.
                    throw std::invalid_argument(
                        "unrecognized version constraint operator in \"" + alt_str + "\"");
                }
            } else {
                alt.name = alt_str;
            }
            if (!alt.name.empty()) spec.alternatives.push_back(alt);
        }
        return spec;
    }

    std::string to_string() const {
        std::string out;
        for (size_t i = 0; i < alternatives.size(); i++) {
            if (i) out += "|";
            out += alternatives[i].to_string();
        }
        return out;
    }
};

inline std::vector<DepSpec> parse_dep_list(const std::string& field) {
    std::vector<DepSpec> out;
    std::stringstream ss(field);
    std::string group;
    while (std::getline(ss, group, ',')) {
        if (group.empty()) continue;
        DepSpec spec = DepSpec::parse(group);
        if (!spec.alternatives.empty()) out.push_back(spec);
    }
    return out;
}

inline std::string format_dep_list(const std::vector<DepSpec>& specs) {
    std::string out;
    for (size_t i = 0; i < specs.size(); i++) {
        if (i) out += ",";
        out += specs[i].to_string();
    }
    return out;
}

struct PackageInfo {
    std::string name;
    std::string version;
    int release = 1;
    std::string arch;
    std::string description;
    std::string license;
    std::vector<DepSpec> depends;       // each entry may itself hold "A|B|C" alternatives
    std::vector<std::string> provides;  // virtual/alias names this package satisfies
    std::vector<DepSpec> conflicts;     // packages that cannot be installed alongside this one
    std::vector<DepSpec> breaks;        // packages this one breaks if present at a bad version
    std::vector<std::string> files;

    // "explicit" = the user directly asked for this package; "auto" = it was
    // pulled in only to satisfy someone else's dependency. Orphan detection/
    // autoremove is exactly "auto packages nothing depends on anymore".
    std::string install_reason = "explicit";
    bool pinned = false;           // held at current version; upgrade() skips it
    uint64_t installed_size = 0;   // bytes, sum of every extracted file
    uint64_t download_size = 0;    // bytes, size of the .wsh that was fetched

    static std::vector<std::string> dep_specs_to_strings(const std::vector<DepSpec>& specs) {
        std::vector<std::string> out;
        for (const auto& s : specs) out.push_back(s.to_string());
        return out;
    }

    static std::vector<DepSpec> dep_specs_from_json(const json& j) {
        std::vector<DepSpec> out;
        for (const auto& s : j) out.push_back(DepSpec::parse(s.get<std::string>()));
        return out;
    }

    json to_json() const {
        json j;
        j["name"] = name;
        j["version"] = version;
        j["release"] = release;
        j["arch"] = arch;
        j["description"] = description;
        j["license"] = license;
        j["depends"] = dep_specs_to_strings(depends);
        j["provides"] = provides;
        j["conflicts"] = dep_specs_to_strings(conflicts);
        j["breaks"] = dep_specs_to_strings(breaks);
        j["files"] = files;
        j["install_reason"] = install_reason;
        j["pinned"] = pinned;
        j["installed_size"] = installed_size;
        j["download_size"] = download_size;
        return j;
    }

    static PackageInfo from_json(const json& j) {
        PackageInfo pkg;
        pkg.name = j.value("name", "");
        pkg.version = j.value("version", "");
        pkg.release = j.value("release", 1);
        pkg.arch = j.value("arch", "");
        pkg.description = j.value("description", "");
        pkg.license = j.value("license", "");
        // Each "depends" entry used to be a bare name string (no version) --
        // DepSpec::parse("name") still parses that correctly (one
        // alternative, no constraint), so old installed.json/index entries
        // keep working with no migration step.
        if (j.contains("depends")) pkg.depends = dep_specs_from_json(j["depends"]);
        if (j.contains("provides")) {
            for (const auto& p : j["provides"]) pkg.provides.push_back(p);
        }
        if (j.contains("conflicts")) pkg.conflicts = dep_specs_from_json(j["conflicts"]);
        if (j.contains("breaks")) pkg.breaks = dep_specs_from_json(j["breaks"]);
        if (j.contains("files")) {
            for (const auto& f : j["files"]) pkg.files.push_back(f);
        }
        pkg.install_reason = j.value("install_reason", "explicit");
        pkg.pinned = j.value("pinned", false);
        pkg.installed_size = j.value("installed_size", (uint64_t)0);
        pkg.download_size = j.value("download_size", (uint64_t)0);
        return pkg;
    }

    std::string filename() const {
        return name + "-" + version + "-" + std::to_string(release) + "-" + arch + ".wsh";
    }
};

// Human-readable byte count (1.2 MB, 340 KB, ...) -- used by every size-
// reporting feature (installed/download size, doctor, etc).
inline std::string human_size(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    char buf[32];
    snprintf(buf, sizeof(buf), (u == 0) ? "%.0f %s" : "%.1f %s", v, units[u]);
    return std::string(buf);
}

struct HistoryEntry {
    std::string timestamp;
    std::string operation;
    std::string package;
    std::vector<std::string> dependencies;
    std::vector<std::string> changed_files;
    std::string status;
    std::string error_msg;
    int generation_id = 0;
    
    json to_json() const {
        json j;
        j["timestamp"] = timestamp;
        j["operation"] = operation;
        j["package"] = package;
        j["dependencies"] = dependencies;
        j["changed_files"] = changed_files;
        j["status"] = status;
        j["error"] = error_msg;
        j["generation"] = generation_id;
        return j;
    }
};

struct ServiceDef {
    std::string name;
    std::string command;
    bool enabled = false;
    std::string restart_policy;
    std::vector<std::string> dependencies;
    std::string log_file;
    pid_t pid = -1;
    int restart_count = 0;
    
    json to_json() const {
        json j;
        j["name"] = name;
        j["command"] = command;
        j["enabled"] = enabled;
        j["restart"] = restart_policy;
        j["dependencies"] = dependencies;
        j["log_file"] = log_file;
        return j;
    }
    
    static ServiceDef from_json(const json& j) {
        ServiceDef s;
        s.name = j.value("name", "");
        s.command = j.value("command", "");
        s.enabled = j.value("enabled", false);
        s.restart_policy = j.value("restart", "no");
        if (j.contains("dependencies")) {
            for (const auto& d : j["dependencies"]) s.dependencies.push_back(d);
        }
        s.log_file = j.value("log_file", "/var/log/wish/" + s.name + ".log");
        return s;
    }
};

struct Generation {
    int id;
    std::string timestamp;
    std::map<std::string, PackageInfo> packages;
    std::vector<std::string> system_files;
    
    json to_json() const {
        json j;
        j["id"] = id;
        j["timestamp"] = timestamp;
        j["packages"] = json::object();
        for (const auto& [name, pkg] : packages) {
            j["packages"][name] = pkg.to_json();
        }
        j["system_files"] = system_files;
        return j;
    }
    
    static Generation from_json(const json& j) {
        Generation g;
        g.id = j.value("id", 0);
        g.timestamp = j.value("timestamp", "");
        if (j.contains("packages")) {
            for (const auto& [name, data] : j["packages"].items()) {
                g.packages[name] = PackageInfo::from_json(data);
            }
        }
        if (j.contains("system_files")) {
            for (const auto& f : j["system_files"]) g.system_files.push_back(f);
        }
        return g;
    }
};

// ============================================================
// Layer Manager (Bedrock-style strata)
// ============================================================

// A share is a host (or cross-layer) path bind-mounted into the layer at run
// time -- this is how a layer gets a shared /home, controlled access to host
// subtrees, or another layer's files.
struct LayerShare {
    std::string source;   // path on host / in another layer
    std::string target;   // absolute path inside the layer
    bool ro = false;

    json to_json() const {
        return json{{"source", source}, {"target", target}, {"ro", ro}};
    }
    static LayerShare from_json(const json& j) {
        LayerShare s;
        s.source = j.value("source", "");
        s.target = j.value("target", "");
        s.ro = j.value("ro", false);
        return s;
    }
};

// Per-layer configuration, stored next to the layer dir as <name>.layer.json.
struct LayerConfig {
    std::vector<LayerShare> shares;
    bool overlay_enabled = false;
    std::vector<std::string> overlay_lowers; // read-only base dirs, high->low priority
    bool gui = false;                        // bind X11/Wayland/PipeWire/D-Bus sockets

    json to_json() const {
        json j;
        j["shares"] = json::array();
        for (const auto& s : shares) j["shares"].push_back(s.to_json());
        j["overlay"] = json{{"enabled", overlay_enabled}, {"lowers", overlay_lowers}};
        j["gui"] = gui;
        return j;
    }
    static LayerConfig from_json(const json& j) {
        LayerConfig c;
        if (j.contains("shares"))
            for (const auto& s : j["shares"]) c.shares.push_back(LayerShare::from_json(s));
        if (j.contains("overlay")) {
            c.overlay_enabled = j["overlay"].value("enabled", false);
            if (j["overlay"].contains("lowers"))
                for (const auto& l : j["overlay"]["lowers"]) c.overlay_lowers.push_back(l);
        }
        c.gui = j.value("gui", false);
        return c;
    }
};

class LayerManager {
    std::string layers_dir_;

    std::string layer_path(const std::string& name) const { return layers_dir_ + "/" + name; }
    std::string config_path(const std::string& name) const { return layers_dir_ + "/" + name + ".layer.json"; }
    // For overlay layers: writable content lives in upper/, transient state in work/, mountpoint merged/.
    std::string upper_path(const std::string& name) const { return layer_path(name) + "/upper"; }
    std::string work_path(const std::string& name)  const { return layer_path(name) + "/work"; }
    std::string merged_path(const std::string& name) const { return layer_path(name) + "/merged"; }

    LayerConfig load_config(const std::string& name) {
        std::string p = config_path(name);
        if (!fs::exists(p)) return LayerConfig{};
        try {
            std::ifstream f(p);
            json j;
            f >> j;
            return LayerConfig::from_json(j);
        } catch (...) {
            return LayerConfig{};
        }
    }

    Result<void> save_config(const std::string& name, const LayerConfig& c) {
        if (!write_file_atomic(config_path(name), c.to_json().dump(2)))
            return Result<void>::failure("Cannot write layer config");
        return Result<void>::success();
    }

    // Reject targets that could escape the layer root once joined.
    static bool safe_target(const std::string& t) {
        if (t.empty() || t[0] != '/') return false;
        if (t.find("..") != std::string::npos) return false;
        if (t.find('\n') != std::string::npos) return false;
        return true;
    }

    std::string wrappers_dir_;
    std::string federation_file_;
    std::string fed_lib_dir_;

    std::vector<std::string> all_layer_names() {
        std::vector<std::string> names;
        std::error_code ec;
        for (const auto& p : fs::directory_iterator(layers_dir_, ec)) {
            if (fs::is_directory(p)) names.push_back(p.path().filename().string());
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    // Priority order of layers for federation conflict resolution. Layers not
    // listed are appended (alphabetically) so every layer still federates.
    std::vector<std::string> load_priority() {
        std::vector<std::string> prio;
        if (fs::exists(federation_file_)) {
            try {
                std::ifstream f(federation_file_);
                json j; f >> j;
                if (j.contains("priority"))
                    for (const auto& l : j["priority"]) prio.push_back(l);
            } catch (...) {}
        }
        for (const auto& n : all_layer_names())
            if (std::find(prio.begin(), prio.end(), n) == prio.end()) prio.push_back(n);
        return prio;
    }

public:
    LayerManager()
        : layers_dir_(Config::get_lib_dir() + "/layers"),
          wrappers_dir_(Config::get_wrappers_dir()),
          federation_file_(Config::get_lib_dir() + "/federation.json"),
          fed_lib_dir_(Config::get_lib_dir() + "/federation/lib") {}

    Result<void> initialize() {
        try {
            fs::create_directories(layers_dir_);
            return Result<void>::success();
        } catch (const fs::filesystem_error& e) {
            return Result<void>::failure("Cannot create layers directory");
        }
    }

    Result<void> add(const std::string& name) {
        if (!PathValidator::is_safe(name)) {
            return Result<void>::failure("Invalid layer name");
        }
        try {
            fs::create_directories(layer_path(name));
            // Seed a default config that shares the host /home (Bedrock's global
            // /home), so a new layer sees the user's home out of the box.
            LayerConfig c;
            c.shares.push_back({"/home", "/home", false});
            save_config(name, c);
            Logger::ok("Layer added: " + name + " (shares /home by default)");
            return Result<void>::success();
        } catch (const fs::filesystem_error& e) {
            return Result<void>::failure("Cannot create layer");
        }
    }

    Result<void> remove(const std::string& name) {
        if (!PathValidator::is_safe(name)) {
            return Result<void>::failure("Invalid layer name");
        }
        std::string path = layer_path(name);
        if (!fs::exists(path)) {
            return Result<void>::failure("Layer not found: " + name);
        }
        try {
            fs::remove_all(path);
            std::error_code ec;
            fs::remove(config_path(name), ec);
            Logger::ok("Layer removed: " + name);
            return Result<void>::success();
        } catch (const fs::filesystem_error& e) {
            return Result<void>::failure("Cannot remove layer");
        }
    }

    Result<void> list() {
        std::cout << "WishOS Layers:\n\n";
        try {
            for (const auto& p : fs::directory_iterator(layers_dir_)) {
                if (fs::is_directory(p)) {
                    std::string name = p.path().filename().string();
                    LayerConfig c = load_config(name);
                    std::cout << " + " << name;
                    if (c.overlay_enabled) std::cout << "  [overlay]";
                    if (!c.shares.empty()) std::cout << "  (" << c.shares.size() << " shares)";
                    std::cout << "\n";
                }
            }
            return Result<void>::success();
        } catch (const fs::filesystem_error& e) {
            return Result<void>::success(); // empty is ok
        }
    }

    Result<void> info(const std::string& name) {
        if (!PathValidator::is_safe(name)) return Result<void>::failure("Invalid layer name");
        if (!fs::exists(layer_path(name))) return Result<void>::failure("Layer not found: " + name);
        LayerConfig c = load_config(name);
        std::cout << "Layer: " << name << "\n";
        std::cout << "  root:    " << layer_path(name) << "\n";
        std::cout << "  gui:     " << (c.gui ? "on (X11/Wayland/PipeWire/D-Bus)" : "off") << "\n";
        std::cout << "  overlay: " << (c.overlay_enabled ? "on" : "off") << "\n";
        if (c.overlay_enabled) {
            for (const auto& l : c.overlay_lowers) std::cout << "    lower: " << l << "\n";
            std::cout << "    upper: " << upper_path(name) << " (copy-on-write)\n";
        }
        std::cout << "  shares:\n";
        if (c.shares.empty()) std::cout << "    (none)\n";
        for (const auto& s : c.shares)
            std::cout << "    " << s.source << " -> " << s.target << (s.ro ? " [ro]" : " [rw]") << "\n";
        return Result<void>::success();
    }

    // Toggle graphical socket sharing (X11 / Wayland / PipeWire / D-Bus).
    Result<void> gui_toggle(const std::string& name, bool on) {
        if (!PathValidator::is_safe(name)) return Result<void>::failure("Invalid layer name");
        if (!fs::exists(layer_path(name))) return Result<void>::failure("Layer not found: " + name);
        LayerConfig c = load_config(name);
        c.gui = on;
        auto r = save_config(name, c);
        if (!r) return r;
        Logger::ok(std::string("GUI sharing ") + (on ? "enabled" : "disabled") + " for " + name);
        return Result<void>::success();
    }

    // Global executable wrapper: drop a tiny script on the host PATH that
    // transparently runs <binary> inside <layer> ("wish run <layer> <binary>").
    // This is how Bedrock makes a stratum's command usable from anywhere.
    Result<void> expose(const std::string& layer, const std::string& binary, const std::string& alias_in) {
        if (!PathValidator::is_safe(layer)) return Result<void>::failure("Invalid layer name");
        if (!PathValidator::is_safe_filename(binary)) return Result<void>::failure("Invalid binary name");
        if (!fs::exists(layer_path(layer))) return Result<void>::failure("Layer not found: " + layer);

        std::string alias = alias_in.empty() ? binary : alias_in;
        if (!PathValidator::is_safe_filename(alias)) return Result<void>::failure("Invalid alias name");

        // Resolve our own absolute path so the wrapper doesn't depend on PATH.
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        std::string wish_bin = (n > 0) ? std::string(buf, n) : "wish";

        std::error_code ec;
        fs::create_directories(wrappers_dir_, ec);
        std::string wrapper = wrappers_dir_ + "/" + alias;

        std::string script =
            "#!/bin/sh\n"
            "# wish-wrapper layer=" + layer + " bin=" + binary + "\n"
            "exec \"" + wish_bin + "\" run " + layer + " " + binary + " \"$@\"\n";

        if (!write_file_atomic(wrapper, script))
            return Result<void>::failure("Cannot write wrapper: " + wrapper);
        if (chmod(wrapper.c_str(), 0755) != 0)
            return Result<void>::failure("Cannot chmod wrapper: " + std::string(strerror(errno)));

        Logger::ok("Exposed " + layer + ":" + binary + " as '" + alias + "' -> " + wrapper);
        return Result<void>::success();
    }

    Result<void> unexpose(const std::string& alias) {
        if (!PathValidator::is_safe_filename(alias)) return Result<void>::failure("Invalid alias name");
        std::string wrapper = wrappers_dir_ + "/" + alias;
        if (!fs::exists(wrapper)) return Result<void>::failure("No such wrapper: " + alias);

        // Only remove files we created (identified by the marker), never an
        // unrelated binary that happens to share the name.
        std::ifstream f(wrapper);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (content.find("# wish-wrapper") == std::string::npos)
            return Result<void>::failure("Not a wish wrapper (refusing to remove): " + wrapper);

        std::error_code ec;
        fs::remove(wrapper, ec);
        if (ec) return Result<void>::failure("Cannot remove wrapper: " + wrapper);
        Logger::ok("Unexposed: " + alias);
        return Result<void>::success();
    }

    // ---- Federation (Bedrock-style cross-layer command & library sharing) ----

    Result<void> priority_show() {
        auto prio = load_priority();
        std::cout << "Layer priority (highest first):\n";
        if (prio.empty()) { std::cout << "  (no layers)\n"; return Result<void>::success(); }
        int rank = 1;
        for (const auto& l : prio) std::cout << "  " << rank++ << ". " << l << "\n";
        return Result<void>::success();
    }

    Result<void> priority_set(const std::vector<std::string>& order) {
        for (const auto& l : order) {
            if (!PathValidator::is_safe(l)) return Result<void>::failure("Invalid layer name: " + l);
            if (!fs::exists(layer_path(l))) return Result<void>::failure("Layer not found: " + l);
        }
        json j;
        j["priority"] = order;
        if (!write_file_atomic(federation_file_, j.dump(2)))
            return Result<void>::failure("Cannot write federation config");
        Logger::ok("Priority order updated");
        return priority_show();
    }

    // Command federation: expose every executable from every layer onto the
    // host PATH, with name collisions resolved by layer priority (the first
    // layer in priority order that provides a command wins). Library
    // federation: aggregate the layers' lib dirs into one federated dir with
    // priority-resolved symlinks + an ld.so.conf.d snippet.
    Result<void> federate() {
        auto root_check = Security::require_root("Federation");
        if (!root_check) return root_check;

        auto prio = load_priority();
        if (prio.empty()) return Result<void>::failure("No layers to federate");

        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        std::string wish_bin = (n > 0) ? std::string(buf, n) : "wish";

        std::error_code ec;
        fs::create_directories(wrappers_dir_, ec);
        fs::create_directories(fed_lib_dir_, ec);

        // --- Command federation ---
        std::set<std::string> claimed_cmd;
        int cmd_count = 0;
        const char* bindirs[] = {"usr/bin", "bin", "usr/sbin", "sbin"};
        for (const auto& layer : prio) {
            for (const char* bd : bindirs) {
                fs::path dir = fs::path(layer_path(layer)) / bd;
                if (!fs::exists(dir)) continue;
                for (const auto& e : fs::directory_iterator(dir, ec)) {
                    std::string cmd = e.path().filename().string();
                    if (!PathValidator::is_safe_filename(cmd)) continue;
                    // executable? (follows symlinks, e.g. busybox applets)
                    std::error_code pe;
                    auto st = fs::status(e.path(), pe);
                    if (pe) continue;
                    if ((st.permissions() & fs::perms::owner_exec) == fs::perms::none) continue;
                    if (claimed_cmd.count(cmd)) continue; // higher-priority layer already won
                    claimed_cmd.insert(cmd);

                    std::string script =
                        "#!/bin/sh\n"
                        "# wish-federated layer=" + layer + " bin=" + cmd + "\n"
                        "exec \"" + wish_bin + "\" run " + layer + " " + cmd + " \"$@\"\n";
                    std::string wpath = wrappers_dir_ + "/" + cmd;
                    if (write_file_atomic(wpath, script)) { chmod(wpath.c_str(), 0755); cmd_count++; }
                }
            }
        }

        // --- Library federation ---
        std::set<std::string> claimed_lib;
        int lib_count = 0;
        const char* libdirs[] = {"usr/lib", "lib", "usr/lib64", "lib64"};
        for (const auto& layer : prio) {
            for (const char* ld : libdirs) {
                fs::path dir = fs::path(layer_path(layer)) / ld;
                if (!fs::exists(dir)) continue;
                for (const auto& e : fs::directory_iterator(dir, ec)) {
                    std::string lib = e.path().filename().string();
                    if (lib.find(".so") == std::string::npos) continue;
                    if (claimed_lib.count(lib)) continue;
                    claimed_lib.insert(lib);
                    fs::path link = fs::path(fed_lib_dir_) / lib;
                    std::error_code le;
                    fs::remove(link, le);
                    fs::create_symlink(e.path(), link, le);
                    if (!le) lib_count++;
                }
            }
        }

        // Wire the federated libs into the dynamic linker if the host uses
        // ld.so.conf.d (harmless no-op otherwise; run ldconfig to apply).
        if (fs::exists("/etc/ld.so.conf.d")) {
            write_file_atomic("/etc/ld.so.conf.d/wish-federation.conf", fed_lib_dir_ + "\n");
        }

        Logger::ok("Federated " + std::to_string(cmd_count) + " commands, " +
                   std::to_string(lib_count) + " libraries");
        Logger::info("Commands -> " + wrappers_dir_ + " | libs -> " + fed_lib_dir_ +
                     " (run 'ldconfig' to refresh the linker cache)");
        return Result<void>::success();
    }

    Result<void> defederate() {
        auto root_check = Security::require_root("Defederation");
        if (!root_check) return root_check;

        int removed = 0;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(wrappers_dir_, ec)) {
            if (!fs::is_regular_file(e.path())) continue;
            std::ifstream f(e.path());
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (content.find("# wish-federated") != std::string::npos) {
                std::error_code re; fs::remove(e.path(), re);
                if (!re) removed++;
            }
        }
        fs::remove_all(fed_lib_dir_, ec);
        fs::remove("/etc/ld.so.conf.d/wish-federation.conf", ec);
        Logger::ok("Defederated (" + std::to_string(removed) + " command wrappers removed)");
        return Result<void>::success();
    }

    // Bind-mount a host / cross-layer path into the layer at run time.
    Result<void> share(const std::string& name, const std::string& source,
                       const std::string& target_in, bool ro) {
        if (!PathValidator::is_safe(name)) return Result<void>::failure("Invalid layer name");
        if (!fs::exists(layer_path(name))) return Result<void>::failure("Layer not found: " + name);
        if (source.empty() || source[0] != '/') return Result<void>::failure("Share source must be an absolute path");
        std::string target = target_in.empty() ? source : target_in;
        if (!safe_target(target)) return Result<void>::failure("Invalid share target: " + target);

        LayerConfig c = load_config(name);
        // Replace an existing share with the same target, else append.
        bool replaced = false;
        for (auto& s : c.shares) {
            if (s.target == target) { s.source = source; s.ro = ro; replaced = true; break; }
        }
        if (!replaced) c.shares.push_back({source, target, ro});
        auto r = save_config(name, c);
        if (!r) return r;
        Logger::ok("Share added: " + source + " -> " + target + (ro ? " [ro]" : " [rw]"));
        return Result<void>::success();
    }

    Result<void> unshare_target(const std::string& name, const std::string& target) {
        if (!PathValidator::is_safe(name)) return Result<void>::failure("Invalid layer name");
        LayerConfig c = load_config(name);
        size_t before = c.shares.size();
        c.shares.erase(std::remove_if(c.shares.begin(), c.shares.end(),
                        [&](const LayerShare& s){ return s.target == target; }), c.shares.end());
        if (c.shares.size() == before) return Result<void>::failure("No share at target: " + target);
        auto r = save_config(name, c);
        if (!r) return r;
        Logger::ok("Share removed: " + target);
        return Result<void>::success();
    }

    // Configure an OverlayFS merge: the layer's writable upper/ over one or more
    // read-only lower dirs (another layer, or specific host subtrees). Writes go
    // to upper as copy-on-write; the lowers are never modified.
    Result<void> overlay(const std::string& name, const std::string& action, const std::string& lower) {
        if (!PathValidator::is_safe(name)) return Result<void>::failure("Invalid layer name");
        if (!fs::exists(layer_path(name))) return Result<void>::failure("Layer not found: " + name);
        LayerConfig c = load_config(name);

        if (action == "add") {
            if (lower.empty() || lower[0] != '/') return Result<void>::failure("Lower dir must be an absolute path");
            if (!fs::exists(lower)) return Result<void>::failure("Lower dir does not exist: " + lower);
            // Guard against the classic overlayfs "upperdir inside lowerdir" error.
            std::string up = fs::weakly_canonical(upper_path(name)).string();
            std::string lo = fs::weakly_canonical(lower).string();
            if (up.compare(0, lo.size(), lo) == 0 && (up.size() == lo.size() || up[lo.size()] == '/'))
                return Result<void>::failure("Lower dir must not contain the layer's upper dir: " + lower);
            c.overlay_lowers.push_back(lower);
            c.overlay_enabled = true;
            std::error_code ec;
            fs::create_directories(upper_path(name), ec);
            auto r = save_config(name, c);
            if (!r) return r;
            Logger::ok("Overlay lower added: " + lower);
            return Result<void>::success();
        } else if (action == "off") {
            c.overlay_enabled = false;
            auto r = save_config(name, c);
            if (!r) return r;
            Logger::ok("Overlay disabled for " + name);
            return Result<void>::success();
        } else {
            return info(name);
        }
    }

    // Copy a layer (tree + config) to a new name. Transient overlay state
    // (work/merged) is not copied.
    Result<void> clone(const std::string& src, const std::string& dst) {
        if (!PathValidator::is_safe(src) || !PathValidator::is_safe(dst))
            return Result<void>::failure("Invalid layer name");
        if (!fs::exists(layer_path(src))) return Result<void>::failure("Source layer not found: " + src);
        if (fs::exists(layer_path(dst))) return Result<void>::failure("Destination layer already exists: " + dst);
        try {
            fs::copy(layer_path(src), layer_path(dst),
                     fs::copy_options::recursive | fs::copy_options::copy_symlinks);
            std::error_code ec;
            fs::remove_all(work_path(dst), ec);
            fs::remove_all(merged_path(dst), ec);
            if (fs::exists(config_path(src)))
                fs::copy_file(config_path(src), config_path(dst), fs::copy_options::overwrite_existing, ec);
            Logger::ok("Layer cloned: " + src + " -> " + dst);
            return Result<void>::success();
        } catch (const fs::filesystem_error& e) {
            return Result<void>::failure(std::string("Clone failed: ") + e.what());
        }
    }

    Result<void> snapshot(const std::string& name, const std::string& snap_in) {
        std::string snap = snap_in;
        if (snap.empty()) {
            std::string ts = Logger::timestamp();
            std::replace(ts.begin(), ts.end(), ':', '-');
            snap = name + "-snap-" + ts;
        }
        return clone(name, snap);
    }

    // Real isolation: private mount namespace + chroot. Optionally an OverlayFS
    // merged root (COW), plus configured share bind-mounts and /proc,/dev,/sys.
    // The mount namespace is per-child, so the kernel tears every mount down on
    // exit -- nothing leaks onto the host.
    Result<void> run(const std::string& layer, const std::vector<std::string>& args) {
        if (!PathValidator::is_safe(layer)) {
            return Result<void>::failure("Invalid layer name");
        }
        if (args.empty()) {
            return Result<void>::failure("No command given");
        }

        auto root_check = Security::require_root("Layer execution");
        if (!root_check) return root_check;

        std::string layer_root = layer_path(layer);
        if (!fs::exists(layer_root)) {
            return Result<void>::failure("Layer not found: " + layer);
        }

        LayerConfig cfg = load_config(layer);
        Logger::info("Entering layer: " + layer + (cfg.overlay_enabled ? " (overlay)" : ""));

        std::vector<const char*> cmd_args;
        for (const auto& arg : args) {
            cmd_args.push_back(arg.c_str());
        }
        cmd_args.push_back(nullptr);

        pid_t pid = fork();
        if (pid == -1) {
            return Result<void>::failure("fork() failed");
        }

        if (pid == 0) {
            if (unshare(CLONE_NEWNS) != 0) {
                std::cerr << "[error] unshare(CLONE_NEWNS) failed: " << strerror(errno)
                          << " (real layer isolation requires CAP_SYS_ADMIN)\n";
                _exit(1);
            }
            if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
                std::cerr << "[error] Failed to make mount tree private: " << strerror(errno) << "\n";
                _exit(1);
            }

            // Determine the root to chroot into: either the plain layer dir, or
            // an OverlayFS merge of the configured lowers with the layer's upper.
            std::string root = layer_root;
            if (cfg.overlay_enabled && !cfg.overlay_lowers.empty()) {
                std::error_code ec;
                fs::create_directories(upper_path(layer), ec);
                fs::create_directories(work_path(layer), ec);
                fs::create_directories(merged_path(layer), ec);

                std::string lowerdirs;
                for (size_t i = 0; i < cfg.overlay_lowers.size(); i++) {
                    if (i) lowerdirs += ":";
                    lowerdirs += cfg.overlay_lowers[i];
                }
                std::string opts = "lowerdir=" + lowerdirs +
                                   ",upperdir=" + upper_path(layer) +
                                   ",workdir=" + work_path(layer);
                if (mount("overlay", merged_path(layer).c_str(), "overlay", 0, opts.c_str()) != 0) {
                    std::cerr << "[error] OverlayFS mount failed: " << strerror(errno)
                              << " (upperdir must be on a filesystem that supports it)\n";
                    _exit(1);
                }
                root = merged_path(layer);
            }

            // Build the full share list: explicit shares + (if gui) the
            // graphical/audio sockets so GUI apps in the layer reach the host
            // display. DISPLAY / WAYLAND_DISPLAY / XDG_RUNTIME_DIR are inherited
            // from the parent's environment automatically (we only reset PATH),
            // so binding the socket paths is all that's needed.
            std::vector<LayerShare> all_shares = cfg.shares;
            if (cfg.gui) {
                all_shares.push_back({"/tmp/.X11-unix", "/tmp/.X11-unix", false});
                const char* xdg = getenv("XDG_RUNTIME_DIR");
                if (xdg && *xdg) all_shares.push_back({xdg, xdg, false}); // Wayland, PipeWire, session D-Bus
                if (fs::exists("/run/dbus/system_bus_socket"))
                    all_shares.push_back({"/run/dbus/system_bus_socket", "/run/dbus/system_bus_socket", false});
            }

            // Apply shares (shared /home, host subtrees, cross-layer, GUI sockets).
            for (const auto& s : all_shares) {
                if (!fs::exists(s.source)) {
                    std::cerr << "[warn] Share source missing, skipping: " << s.source << "\n";
                    continue;
                }
                std::string tgt = root + s.target;
                std::error_code ec;
                if (fs::is_directory(s.source)) fs::create_directories(tgt, ec);
                else { fs::create_directories(fs::path(tgt).parent_path(), ec); std::ofstream(tgt).flush(); }

                if (mount(s.source.c_str(), tgt.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
                    std::cerr << "[warn] Failed to bind share " << s.source << ": " << strerror(errno) << "\n";
                    continue;
                }
                if (s.ro) {
                    // A read-only bind needs a second remount pass.
                    if (mount(nullptr, tgt.c_str(), nullptr,
                              MS_BIND | MS_REMOUNT | MS_RDONLY, nullptr) != 0) {
                        std::cerr << "[warn] Failed to remount share read-only: " << s.target << "\n";
                    }
                }
            }

            struct BindMount { const char* src; const char* rel; bool required; };
            BindMount binds[] = {
                {"/proc", "/proc", true},
                {"/sys",  "/sys",  false},
                {"/dev",  "/dev",  false},
            };
            for (const auto& b : binds) {
                std::string target = root + b.rel;
                std::error_code ec;
                fs::create_directories(target, ec);
                if (mount(b.src, target.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
                    if (b.required) {
                        std::cerr << "[error] Failed to bind mount " << b.src << ": " << strerror(errno) << "\n";
                        _exit(1);
                    }
                    std::cerr << "[warn] Failed to bind mount " << b.src << ": " << strerror(errno) << "\n";
                }
            }

            if (chroot(root.c_str()) != 0) {
                std::cerr << "[error] chroot failed: " << strerror(errno) << "\n";
                _exit(1);
            }
            if (chdir("/") != 0) {
                std::cerr << "[error] Cannot chdir into layer root\n";
                _exit(1);
            }

            setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
            execvp(
                cmd_args[0],
                const_cast<char* const*>(cmd_args.data())
            );
            std::cerr << "[error] execvp failed: " << strerror(errno) << "\n";
            _exit(1);
        } else {
            int status;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                return Result<void>::failure("Command exited with code " + std::to_string(WEXITSTATUS(status)));
            }
            if (WIFSIGNALED(status)) {
                return Result<void>::failure("Command killed by signal " + std::to_string(WTERMSIG(status)));
            }
        }
        return Result<void>::success();
    }
};

// ============================================================
// History Manager
// ============================================================

class HistoryManager {
    std::string history_file_;
    std::vector<HistoryEntry> entries_;
    std::mutex mutex_;
    
    void load() {
        if (!fs::exists(history_file_)) return;
        try {
            std::ifstream f(history_file_);
            json j;
            f >> j;
            for (const auto& entry : j["history"]) {
                HistoryEntry he;
                he.timestamp = entry.value("timestamp", "");
                he.operation = entry.value("operation", "");
                he.package = entry.value("package", "");
                he.status = entry.value("status", "");
                he.error_msg = entry.value("error", "");
                if (entry.contains("dependencies")) {
                    for (const auto& d : entry["dependencies"]) he.dependencies.push_back(d);
                }
                if (entry.contains("changed_files")) {
                    for (const auto& f : entry["changed_files"]) he.changed_files.push_back(f);
                }
                entries_.push_back(he);
            }
        } catch (...) {
            // corrupted history, start fresh
            entries_.clear();
        }
    }
    
    void save() {
        json j;
        j["history"] = json::array();
        for (const auto& e : entries_) {
            j["history"].push_back(e.to_json());
        }
        if (!write_file_atomic(history_file_, j.dump(2))) {
            Logger::warn("Failed to write history file: " + history_file_);
        }
    }
    
public:
    HistoryManager() : history_file_(Config::get_lib_dir() + "/history.json") {
        load();
    }
    
    Result<void> initialize() {
        try {
            fs::create_directories(Config::get_lib_dir());
            return Result<void>::success();
        } catch (const fs::filesystem_error& e) {
            return Result<void>::failure("Cannot create lib directory");
        }
    }
    
    void record(const std::string& operation, const std::string& package,
                const std::vector<std::string>& deps = {},
                const std::vector<std::string>& files = {},
                bool success = true, const std::string& error = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        HistoryEntry he;
        he.timestamp = Logger::timestamp();
        he.operation = operation;
        he.package = package;
        he.dependencies = deps;
        he.changed_files = files;
        he.status = success ? "success" : "failed";
        he.error_msg = error;
        entries_.push_back(he);
        save();
    }
    
    Result<void> show_history() {
        if (entries_.empty()) {
            std::cout << "No transaction history found.\n";
            return Result<void>::success();
        }
        
        std::cout << "WishOS Transaction History\n";
        std::cout << std::string(50, '=') << "\n\n";
        
        for (const auto& e : entries_) {
            std::cout << "[" << e.timestamp << "] ";
            std::cout << e.operation << " " << e.package;
            if (e.status == "failed") {
                std::cout << " [FAILED: " << e.error_msg << "]";
            }
            std::cout << "\n";
            if (!e.dependencies.empty()) {
                std::cout << "  deps: ";
                for (const auto& d : e.dependencies) std::cout << d << " ";
                std::cout << "\n";
            }
        }
        return Result<void>::success();
    }
    
    Result<void> show_graph(const std::string& package) {
        // Build dependency tree from history
        std::cout << "Dependency history for: " << package << "\n\n";
        
        for (const auto& e : entries_) {
            if (e.package == package && e.operation == "install") {
                std::cout << e.package << "\n";
                for (const auto& dep : e.dependencies) {
                    std::cout << "  +-- " << dep << "\n";
                }
            }
        }
        return Result<void>::success();
    }
};

// ============================================================
// Dependency Resolver
// ============================================================

enum class DepState { UNVISITED, VISITING, VISITED };

class RemoteIndex; // defined below; resolver only needs a pointer to it.

// Resolves against RemoteIndex::get() directly (instead of a pre-loaded
// snapshot) so that per-package .info detail (depends/description/license)
// is fetched lazily, only for packages actually reachable from the requested
// install target -- not for the whole catalog. See RemoteIndex::get().
class DependencyResolver {
    RemoteIndex* index_ = nullptr;

public:
    void set_index(RemoteIndex* index) {
        index_ = index;
    }

    Result<std::vector<std::string>> resolve(const std::string& pkg_name);

    Result<std::vector<std::string>> resolve_recursive(
        const std::string& pkg_name,
        std::map<std::string, DepState>& state,
        std::vector<std::string>& resolved);

    // Resolves one dependency slot (a DepSpec, possibly several "A|B|C"
    // alternatives): tries each alternative in order, first as a literal
    // package name, then as a virtual name via the provides index. A
    // version constraint violated by an available candidate is a real
    // finding (not silently skipped) and only falls through to the next
    // alternative rather than failing outright, so "libgcc-s1 (>=3.0) |
    // libgcc1" can still succeed via the second alternative even if the
    // first is present at too old a version.
    Result<std::vector<std::string>> resolve_dep_spec(
        const DepSpec& dep,
        std::map<std::string, DepState>& state,
        std::vector<std::string>& resolved);

    Result<void> show_graph(const std::string& pkg_name, int depth = 0) {
        std::set<std::string> ancestors;
        return show_graph(pkg_name, depth, ancestors);
    }
    Result<void> show_graph(const std::string& pkg_name, int depth, std::set<std::string>& ancestors);
};

// ============================================================
// Lockfile (prevents concurrent wish invocations from racing on
// installed.json / the cache / generations)
// ============================================================

// RAII advisory lock over <lib_dir>/wish.lock. flock() releases
// automatically if the holding process dies (crash, kill -9, power loss
// mid-op) -- unlike a plain pidfile-based lock, there's no stale-lock
// cleanup step needed.
class Lockfile {
    int fd_ = -1;
public:
    Result<void> acquire() {
        std::string path = Config::get_lib_dir() + "/wish.lock";
        std::error_code ec;
        fs::create_directories(Config::get_lib_dir(), ec);
        fd_ = open(path.c_str(), O_CREAT | O_RDWR, 0644);
        if (fd_ < 0) return Result<void>::failure("Cannot open lockfile: " + path);
        if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            close(fd_);
            fd_ = -1;
            return Result<void>::failure(
                "Another wish operation is already running (lockfile held)");
        }
        return Result<void>::success();
    }

    ~Lockfile() {
        if (fd_ >= 0) { flock(fd_, LOCK_UN); close(fd_); }
    }
};

// ============================================================
// Package Database
// ============================================================

class PackageDatabase {
    std::string db_file_;
    std::map<std::string, PackageInfo> installed_;
    std::mutex mutex_;
    
    void load() {
        if (!fs::exists(db_file_)) return;
        try {
            std::ifstream f(db_file_);
            json j;
            f >> j;
            for (const auto& [name, data] : j["installed"].items()) {
                // Per-entry, not per-file: a single corrupted/malformed
                // entry (e.g. an unrecognized version-constraint operator,
                // see DepSpec::parse) used to wipe the ENTIRE in-memory
                // database on the next load(), and a later save() would
                // then silently persist that empty state -- permanently
                // losing every other, perfectly fine, installed package's
                // record. Skip just the bad entry instead.
                try {
                    installed_[name] = PackageInfo::from_json(data);
                } catch (const std::exception& e) {
                    Logger::error("Skipping corrupt installed.json entry \"" + name +
                                  "\": " + e.what());
                }
            }
        } catch (...) {
            installed_.clear();
        }
    }
    
    void save() {
        json j;
        j["installed"] = json::object();
        for (const auto& [name, pkg] : installed_) {
            j["installed"][name] = pkg.to_json();
        }
        if (!write_file_atomic(db_file_, j.dump(2))) {
            Logger::warn("Failed to write package database: " + db_file_);
        }
    }
    
public:
    PackageDatabase() : db_file_(Config::get_lib_dir() + "/installed.json") {
        load();
    }
    
    Result<void> initialize() {
        try {
            fs::create_directories(Config::get_lib_dir());
            return Result<void>::success();
        } catch (...) {
            return Result<void>::failure("Cannot create package database");
        }
    }
    
    bool is_installed(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        return installed_.count(name) > 0;
    }
    
    Result<void> install(const PackageInfo& pkg, const std::vector<std::string>& files) {
        std::lock_guard<std::mutex> lock(mutex_);
        PackageInfo p = pkg;
        p.files = files;
        installed_[pkg.name] = p;
        save();
        return Result<void>::success();
    }
    
    Result<void> remove(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        installed_.erase(name);
        save();
        return Result<void>::success();
    }
    
    Result<PackageInfo> get(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = installed_.find(name);
        if (it == installed_.end()) {
            return Result<PackageInfo>::failure("Package not installed: " + name);
        }
        return Result<PackageInfo>::success(it->second);
    }
    
    Result<void> list() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (installed_.empty()) {
            std::cout << "No packages installed.\n";
            return Result<void>::success();
        }
        
        std::cout << "Installed Packages:\n";
        std::cout << std::string(40, '=') << "\n";
        for (const auto& [name, pkg] : installed_) {
            std::cout << name << " " << pkg.version << "-" << pkg.release
                      << "  (" << human_size(pkg.installed_size) << ")"
                      << (pkg.install_reason == "auto" ? " [auto]" : "")
                      << (pkg.pinned ? " [pinned]" : "") << "\n";
        }
        return Result<void>::success();
    }

    std::map<std::string, PackageInfo> get_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        return installed_;
    }

    // "which package owns this file" -- resolves to an absolute path first
    // (installed file lists are stored absolute) so it works regardless of
    // the caller's cwd or whether they passed a relative path.
    Result<std::string> owning_file(const std::string& path) {
        std::error_code ec;
        std::string abs = fs::absolute(path, ec).string();
        if (ec) abs = path;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [name, pkg] : installed_) {
            for (const auto& f : pkg.files) {
                if (f == abs || f == path) return Result<std::string>::success(name);
            }
        }
        return Result<std::string>::failure("No package owns: " + path);
    }

    Result<void> set_pinned(const std::string& name, bool pinned) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = installed_.find(name);
        if (it == installed_.end()) return Result<void>::failure("Package not installed: " + name);
        it->second.pinned = pinned;
        save();
        return Result<void>::success();
    }

    bool is_pinned(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = installed_.find(name);
        return it != installed_.end() && it->second.pinned;
    }

    Result<void> set_install_reason(const std::string& name, const std::string& reason) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = installed_.find(name);
        if (it == installed_.end()) return Result<void>::failure("Package not installed: " + name);
        it->second.install_reason = reason;
        save();
        return Result<void>::success();
    }

    // A dependency's alternative name might not be a literal installed
    // package -- it could be a virtual name some OTHER installed package
    // provides (e.g. "virtual-lib" satisfied by "pkg-provider"). Every
    // graph-walking method below (find_orphans/reverse_depends/why) needs
    // to resolve through that indirection or it'll treat a provides-
    // satisfied dependency as unreachable/untracked. Assumes mutex_ is
    // already held by the caller.
    std::vector<std::string> resolve_installed_provider(const std::string& want) const {
        if (installed_.count(want)) return {want};
        std::vector<std::string> out;
        for (const auto& [name, pkg] : installed_) {
            for (const auto& p : pkg.provides) {
                if (p == want) { out.push_back(name); break; }
            }
        }
        return out;
    }

    // Mark-and-sweep from every explicitly-installed package (the GC
    // "roots"): walk `depends` to find everything reachable, and anything
    // auto-installed that ISN'T reachable is an orphan. This -- not a
    // pairwise "does anything still list me in its depends" elimination --
    // is what correctly handles real circular runtime dependencies (e.g.
    // libc6 <-> libgcc-s1, which genuinely occur in real distro package
    // graphs): a cycle that nothing external reaches is correctly orphaned
    // as a whole, because reachability from the roots is what's being
    // tested, not mutual "do you still need me" bookkeeping between cycle
    // members that would otherwise keep propping each other up forever.
    std::vector<std::string> find_orphans() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::set<std::string> reachable;
        std::vector<std::string> stack;
        for (const auto& [name, pkg] : installed_) {
            if (pkg.install_reason != "auto") stack.push_back(name);
        }
        while (!stack.empty()) {
            std::string name = stack.back();
            stack.pop_back();
            if (!reachable.insert(name).second) continue; // already visited
            auto it = installed_.find(name);
            if (it == installed_.end()) continue;
            for (const auto& dep : it->second.depends) {
                // Reachability doesn't know which alternative was actually
                // chosen at install time (that's not tracked separately),
                // so any alternative in an OR-group counts as "reachable" --
                // and an alternative naming a virtual package resolves
                // through whichever installed package actually provides it.
                for (const auto& alt : dep.alternatives) {
                    for (const auto& real_name : resolve_installed_provider(alt.name)) {
                        if (!reachable.count(real_name)) stack.push_back(real_name);
                    }
                }
            }
        }
        std::vector<std::string> orphans;
        for (const auto& [name, pkg] : installed_) {
            if (pkg.install_reason == "auto" && !reachable.count(name)) orphans.push_back(name);
        }
        return orphans;
    }

    // Packages that directly depend on `name` -- used both by reverse-
    // dependency protection (can't remove X while Y still needs it) and by
    // `wish why <name>`.
    std::vector<std::string> reverse_depends(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> out;
        // `name` itself might satisfy a virtual dependency via its own
        // provides list, not just its literal name.
        std::set<std::string> also_matches = {name};
        auto self_it = installed_.find(name);
        if (self_it != installed_.end()) {
            also_matches.insert(self_it->second.provides.begin(), self_it->second.provides.end());
        }
        for (const auto& [pname, pkg] : installed_) {
            if (pname == name) continue;
            bool needs_it = false;
            for (const auto& d : pkg.depends) {
                for (const auto& alt : d.alternatives) {
                    if (also_matches.count(alt.name)) { needs_it = true; break; }
                }
                if (needs_it) break;
            }
            if (needs_it) out.push_back(pname);
        }
        return out;
    }

    // BFS from every explicitly-installed package (the same "roots" concept
    // find_orphans() marks-and-sweeps from) to the shortest chain that pulled
    // `name` onto the system. Empty chain + true = name is itself explicit
    // (installed on purpose, no chain to show).
    Result<std::vector<std::string>> why(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!installed_.count(name)) {
            return Result<std::vector<std::string>>::failure("Not installed: " + name);
        }
        if (installed_.at(name).install_reason != "auto") {
            return Result<std::vector<std::string>>::success({}); // explicit -- no chain
        }

        std::map<std::string, std::string> parent; // child -> who pulled it in
        std::queue<std::string> q;
        for (const auto& [pname, pkg] : installed_) {
            if (pkg.install_reason != "auto") { q.push(pname); parent[pname] = ""; }
        }
        while (!q.empty()) {
            std::string cur = q.front(); q.pop();
            if (cur == name) {
                std::vector<std::string> chain;
                for (std::string n = name; !n.empty(); n = parent[n]) chain.push_back(n);
                std::reverse(chain.begin(), chain.end());
                return Result<std::vector<std::string>>::success(chain);
            }
            auto it = installed_.find(cur);
            if (it == installed_.end()) continue;
            for (const auto& d : it->second.depends) {
                for (const auto& alt : d.alternatives) {
                    for (const auto& real_name : resolve_installed_provider(alt.name)) {
                        if (!parent.count(real_name)) {
                            parent[real_name] = cur;
                            q.push(real_name);
                        }
                    }
                }
            }
        }
        return Result<std::vector<std::string>>::failure(
            name + " is installed but not reachable from anything explicit (data inconsistency)");
    }
};

// ============================================================
// Generation Manager (NixOS-style system generations)
// ============================================================

// Each generation is a full snapshot of the installed-package set at a point
// in time, stored as <lib>/generations/<id>.json. A plain-text `current` file
// records which generation the live system reflects. Because a snapshot is
// just the DB state, "rolling back" is deterministic: reconcile the live set
// to a recorded one.
class GenerationManager {
    std::string gen_dir_;

    static int parse_id(const std::string& stem) {
        try { return std::stoi(stem); } catch (...) { return -1; }
    }

    std::string config_snap_dir(int id) const {
        return gen_dir_ + "/" + std::to_string(id) + ".config";
    }

    // Mirror src -> dst: make dst exactly match src. Copies changed/new files
    // (and restores deleted ones) then removes anything in dst not in src, so a
    // rollback truly reverts config edits, additions, and deletions.
    static void mirror_dir(const fs::path& src, const fs::path& dst) {
        std::error_code ec;
        fs::create_directories(dst, ec);
        // Pass 1: bring every entry from the snapshot into the live tree.
        for (auto it = fs::recursive_directory_iterator(
                 src, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); ++it) {
            const fs::path& sp = it->path();
            fs::path rel = fs::relative(sp, src, ec);
            if (ec) { ec.clear(); continue; }
            fs::path dp = dst / rel;
            std::error_code e2;
            if (fs::is_symlink(fs::symlink_status(sp, e2))) {
                fs::remove(dp, e2);
                auto target = fs::read_symlink(sp, e2);
                if (!e2) fs::create_symlink(target, dp, e2);
            } else if (fs::is_directory(fs::symlink_status(sp, e2))) {
                fs::create_directories(dp, e2);
            } else {
                fs::create_directories(dp.parent_path(), e2);
                fs::copy_file(sp, dp, fs::copy_options::overwrite_existing, e2);
            }
        }
        // Pass 2: delete entries that exist live but not in the snapshot.
        std::vector<fs::path> to_remove;
        std::error_code ec2;
        for (auto it = fs::recursive_directory_iterator(
                 dst, fs::directory_options::skip_permission_denied, ec2);
             !ec2 && it != fs::recursive_directory_iterator(); ++it) {
            fs::path rel = fs::relative(it->path(), dst, ec2);
            if (ec2) { ec2.clear(); continue; }
            std::error_code e3;
            if (!fs::exists(fs::symlink_status(src / rel, e3)))
                to_remove.push_back(it->path());
        }
        // Remove deepest paths first so directories empty out before removal.
        std::sort(to_remove.begin(), to_remove.end(),
                  [](const fs::path& a, const fs::path& b){ return a.string().size() > b.string().size(); });
        for (const auto& p : to_remove) { std::error_code e; fs::remove(p, e); }
    }

public:
    GenerationManager() : gen_dir_(Config::get_generations_dir()) {}

    Result<void> initialize() {
        std::error_code ec;
        fs::create_directories(gen_dir_, ec);
        if (ec) return Result<void>::failure("Cannot create generations directory");
        return Result<void>::success();
    }

    int current_id() {
        std::ifstream f(gen_dir_ + "/current");
        int id = 0;
        if (f >> id) return id;
        return 0;
    }

    int latest_id() {
        int max_id = 0;
        std::error_code ec;
        for (const auto& p : fs::directory_iterator(gen_dir_, ec)) {
            if (p.path().extension() == ".json") {
                int id = parse_id(p.path().stem().string());
                if (id > max_id) max_id = id;
            }
        }
        return max_id;
    }

    Result<void> set_current(int id) {
        if (!write_file_atomic(gen_dir_ + "/current", std::to_string(id) + "\n"))
            return Result<void>::failure("Cannot update current generation pointer");
        return Result<void>::success();
    }

    Result<int> snapshot(const std::string& op,
                         const std::map<std::string, PackageInfo>& installed,
                         const std::string& root = "/",
                         const std::vector<std::string>& managed = {}) {
        auto init = initialize();
        if (!init) return Result<int>::failure(init.error());

        int id = latest_id() + 1;
        Generation g;
        g.id = id;
        g.timestamp = Logger::timestamp();
        g.packages = installed;

        json j = g.to_json();
        j["operation"] = op;
        j["managed"] = managed;

        if (!write_file_atomic(gen_dir_ + "/" + std::to_string(id) + ".json", j.dump(2)))
            return Result<int>::failure("Cannot write generation file");

        // Capture the managed config subtrees so a future rollback can restore
        // config/filesystem state, not just package-owned files.
        for (const auto& p : managed) {
            fs::path live = fs::path(root) / p;
            std::error_code ec;
            if (!fs::exists(live, ec)) continue;
            fs::path dst = fs::path(config_snap_dir(id)) / p;
            try {
                fs::create_directories(dst.parent_path(), ec);
                fs::copy(live, dst,
                         fs::copy_options::recursive | fs::copy_options::copy_symlinks |
                         fs::copy_options::overwrite_existing);
            } catch (const std::exception& e) {
                Logger::warn(std::string("Config snapshot of ") + p + " incomplete: " + e.what());
            }
        }

        set_current(id);
        return Result<int>::success(id);
    }

    // Restore the managed config subtrees recorded in generation `id`.
    Result<void> restore_config(int id, const std::string& root,
                                const std::vector<std::string>& managed) {
        for (const auto& p : managed) {
            fs::path snap = fs::path(config_snap_dir(id)) / p;
            std::error_code ec;
            if (!fs::exists(snap, ec)) continue;
            fs::path live = fs::path(root) / p;
            Logger::info("Restoring config: " + p);
            mirror_dir(snap, live);
        }
        return Result<void>::success();
    }

    Result<Generation> get(int id) {
        std::string path = gen_dir_ + "/" + std::to_string(id) + ".json";
        if (!fs::exists(path))
            return Result<Generation>::failure("Generation not found: " + std::to_string(id));
        try {
            std::ifstream f(path);
            json j;
            f >> j;
            return Result<Generation>::success(Generation::from_json(j));
        } catch (...) {
            return Result<Generation>::failure("Corrupt generation file: " + std::to_string(id));
        }
    }

    Result<void> list() {
        std::error_code ec;
        std::vector<int> ids;
        for (const auto& p : fs::directory_iterator(gen_dir_, ec)) {
            if (p.path().extension() == ".json") {
                int id = parse_id(p.path().stem().string());
                if (id >= 0) ids.push_back(id);
            }
        }
        if (ids.empty()) {
            std::cout << "No generations yet.\n";
            return Result<void>::success();
        }
        std::sort(ids.begin(), ids.end());
        int cur = current_id();

        std::cout << "WishOS Generations:\n";
        std::cout << std::string(50, '=') << "\n";
        for (int id : ids) {
            auto g = get(id);
            std::string ts = g ? g.value().timestamp : "?";
            size_t n = g ? g.value().packages.size() : 0;
            std::cout << (id == cur ? " * " : "   ")
                      << id << "  " << ts
                      << "  (" << n << " packages)"
                      << (id == cur ? "  [current]" : "") << "\n";
        }
        return Result<void>::success();
    }
};

// ============================================================
// Remote Index Manager
// ============================================================

class RemoteIndex {
    std::map<std::string, PackageInfo> packages_;
    std::set<std::string> info_fetched_;
    std::string cache_file_;
    std::map<std::string, std::vector<std::string>> provides_index_; // virtual name -> real package names
    bool provides_loaded_ = false;

    PackageInfo parse_line(const std::string& line) {
        PackageInfo pkg;
        // Format: name-version-release-arch.wsh
        std::regex pattern("([a-z0-9-]+)-([0-9.]+)-(\\d+)-([a-z0-9_]+)\\.wsh");
        std::smatch match;
        
        if (std::regex_match(line, match, pattern)) {
            pkg.name = match[1].str();
            pkg.version = match[2].str();
            try {
                pkg.release = std::stoi(match[3].str());
            } catch (...) {
                pkg.release = 1;
            }
            pkg.arch = match[4].str();
        }
        return pkg;
    }
    
public:
    RemoteIndex() : cache_file_(Config::get_cache_dir() + "/index-" + Config::get_arch() + ".json") {}
    
    Result<void> fetch() {
        Logger::info("Fetching package index...");
        
        std::string arch = Config::get_arch();
        std::string url = Config::get_repo_url() + "/index/" + arch + ".txt";
        
        auto result = HttpClient::get(url);
        if (!result) {
            // Try cached version
            if (fs::exists(cache_file_)) {
                Logger::warn("Using cached index");
                return load_cache();
            }
            return Result<void>::failure("Failed to fetch index: " + result.error());
        }
        
        // Detailed .info fetches (depends/description/license) are lazy --
        // see get() below. Doing them eagerly for the whole catalog here used
        // to mean one full sequential HTTPS round-trip per package (thousands
        // of them once the Ubuntu mirror was added), which made every fresh
        // `wish install` hang for tens of minutes before it ever resolved a
        // single dependency.
        std::map<std::string, PackageInfo> old_info;
        if (fs::exists(cache_file_)) {
            RemoteIndex cached;
            if (cached.load_cache()) old_info = cached.packages_;
        }

        packages_.clear();
        std::istringstream stream(result.value());
        std::string line;
        while (std::getline(stream, line)) {
            if (line.empty()) continue;
            PackageInfo pkg = parse_line(line);
            if (!pkg.name.empty()) {
                // Only trust a cached entry as "already fetched" if it looks
                // like real fetched data (non-empty description) rather than
                // the blank placeholder every package gets when the index is
                // first parsed -- save_cache() persists the *whole* packages_
                // map on every single per-package fetch, so without this
                // check the very first save would make every other package's
                // blank placeholder look "fetched", and their real depends
                // would never be downloaded again (this exact bug caused
                // iwd's dependency list to silently come back empty).
                auto old = old_info.find(pkg.name);
                if (old != old_info.end() && old->second.version == pkg.version &&
                    old->second.release == pkg.release &&
                    !old->second.description.empty()) {
                    pkg.description = old->second.description;
                    pkg.license = old->second.license;
                    pkg.depends = old->second.depends;
                    pkg.provides = old->second.provides;
                    pkg.conflicts = old->second.conflicts;
                    pkg.breaks = old->second.breaks;
                    info_fetched_.insert(pkg.name);
                }
                packages_[pkg.name] = pkg;
            }
        }

        save_cache();
        Logger::ok("Index updated (" + std::to_string(packages_.size()) + " packages)");
        return Result<void>::success();
    }

    // Returns failure only for genuinely malformed dependency data (an
    // unrecognized version-constraint operator -- see DepSpec::parse) so a
    // corrupted/hand-edited .info can't silently masquerade as "no
    // constraint" and let something unverifiable through. A network/fetch
    // failure is NOT an error here -- callers already treat "no .info
    // available" as the existing tolerant missing-package path.
    Result<void> fetch_package_info(const std::string& name, PackageInfo& pkg) {
        std::string info_url = Config::get_repo_url() + "/pkgs/" + Config::get_arch() + "/" + name + ".info";
        auto result = HttpClient::get(info_url);
        if (!result) return Result<void>::success();

        std::istringstream stream(result.value());
        std::string line;
        try {
            while (std::getline(stream, line)) {
                size_t eq = line.find('=');
                if (eq == std::string::npos) continue;

                std::string key = line.substr(0, eq);
                std::string value = line.substr(eq + 1);

                if (key == "description") pkg.description = value;
                else if (key == "license") pkg.license = value;
                else if (key == "depends") pkg.depends = parse_dep_list(value);
                else if (key == "conflicts") pkg.conflicts = parse_dep_list(value);
                else if (key == "breaks") pkg.breaks = parse_dep_list(value);
                else if (key == "provides") {
                    std::istringstream p_stream(value);
                    std::string pname;
                    while (std::getline(p_stream, pname, ',')) {
                        pname.erase(0, pname.find_first_not_of(" \t"));
                        pname.erase(pname.find_last_not_of(" \t") + 1);
                        if (!pname.empty()) pkg.provides.push_back(pname);
                    }
                }
            }
        } catch (const std::invalid_argument& e) {
            return Result<void>::failure(name + ": malformed dependency data (" + e.what() + ")");
        }
        return Result<void>::success();
    }
    
    void save_cache() {
        json j;
        for (const auto& [name, pkg] : packages_) {
            if (!info_fetched_.count(name)) continue;
            j[name] = pkg.to_json();
        }
        try {
            fs::create_directories(Config::get_cache_dir());
            write_file_atomic(cache_file_, j.dump(2));
        } catch (...) {}
    }
    
    Result<void> load_cache() {
        try {
            std::ifstream f(cache_file_);
            json j;
            f >> j;
            for (const auto& [name, data] : j.items()) {
                // Same reasoning as PackageDatabase::load(): one malformed
                // entry shouldn't invalidate every other cached package,
                // especially since this is also the offline fallback path.
                try {
                    packages_[name] = PackageInfo::from_json(data);
                } catch (const std::exception& e) {
                    Logger::error("Skipping corrupt index cache entry \"" + name +
                                  "\": " + e.what());
                }
            }
            return Result<void>::success();
        } catch (...) {
            return Result<void>::failure("Cannot load cached index");
        }
    }
    
    bool has_package(const std::string& name) const {
        return packages_.count(name) > 0;
    }
    
    Result<PackageInfo> get(const std::string& name) {
        auto it = packages_.find(name);
        if (it == packages_.end()) {
            return Result<PackageInfo>::failure("Package not found: " + name);
        }
        if (!info_fetched_.count(name)) {
            auto fetch_r = fetch_package_info(name, it->second);
            if (!fetch_r) return Result<PackageInfo>::failure(fetch_r.error());
            info_fetched_.insert(name);
            save_cache();
        }
        return Result<PackageInfo>::success(it->second);
    }

    // Virtual-package (Provides:) lookup. Fetched lazily, once, only the
    // first time the resolver actually needs it (a dependency name that
    // doesn't exist as a literal package) -- not at fetch()/get() time for
    // every package, which would mean re-scanning the whole catalog's
    // metadata just to build this map and reintroduce the eager-fetch
    // performance bug fixed earlier. It's a single small file
    // (index/<arch>-provides.txt, "virtual_name real_name" per line, built
    // by the mirror scripts as they process each package) so one fetch
    // covers every virtual-package lookup for the rest of this run.
    std::vector<std::string> get_providers(const std::string& virtual_name) {
        if (!provides_loaded_) {
            provides_loaded_ = true;
            std::string url = Config::get_repo_url() + "/index/" + Config::get_arch() + "-provides.txt";
            auto result = HttpClient::get(url);
            if (result) {
                std::istringstream stream(result.value());
                std::string line;
                while (std::getline(stream, line)) {
                    std::istringstream ls(line);
                    std::string vname, rname;
                    if (ls >> vname >> rname) provides_index_[vname].push_back(rname);
                }
            }
        }
        auto it = provides_index_.find(virtual_name);
        return it != provides_index_.end() ? it->second : std::vector<std::string>{};
    }

    std::map<std::string, PackageInfo> get_all() const {
        return packages_;
    }
    
    Result<void> search(const std::string& query) {
        std::cout << "Search results for \"" << query << "\":\n";
        std::cout << std::string(50, '=') << "\n";
        
        bool found = false;
        for (const auto& [name, pkg] : packages_) {
            if (name.find(query) != std::string::npos || 
                pkg.description.find(query) != std::string::npos) {
                std::cout << name << " " << pkg.version << "-" << pkg.release << "\n";
                if (!pkg.description.empty()) {
                    std::cout << "  " << pkg.description << "\n";
                }
                found = true;
            }
        }
        
        if (!found) {
            std::cout << "No packages found.\n";
        }
        return Result<void>::success();
    }
};

inline Result<std::vector<std::string>> DependencyResolver::resolve(const std::string& pkg_name) {
    std::map<std::string, DepState> state;
    std::vector<std::string> resolved;
    auto result = resolve_recursive(pkg_name, state, resolved);
    if (!result) return result;

    // The per-visit conflict/breaks check in resolve_recursive is
    // order-dependent: it only catches a conflict against whatever was
    // ALREADY in `resolved` at the moment a package is visited, so it can
    // miss one where the conflicting package gets added later (e.g. by the
    // same package's own dependency chain). This final pass checks every
    // package in the completed closure against every other, catching
    // anything the walk order missed.
    const auto& final_set = result.value();
    std::set<std::string> in_set(final_set.begin(), final_set.end());
    for (const auto& name : final_set) {
        auto pkg_r = index_->get(name);
        if (!pkg_r) continue;
        for (const auto& c : pkg_r.value().conflicts) {
            for (const auto& alt : c.alternatives) {
                if (in_set.count(alt.name)) {
                    return Result<std::vector<std::string>>::failure(
                        name + " conflicts with " + alt.name + ", which this install also needs");
                }
            }
        }
        for (const auto& b : pkg_r.value().breaks) {
            for (const auto& alt : b.alternatives) {
                if (in_set.count(alt.name)) {
                    return Result<std::vector<std::string>>::failure(
                        name + " breaks " + alt.name + ", which this install also needs");
                }
            }
        }
    }
    return result;
}

inline Result<std::vector<std::string>> DependencyResolver::resolve_recursive(
    const std::string& pkg_name,
    std::map<std::string, DepState>& state,
    std::vector<std::string>& resolved)
{
    auto it = state.find(pkg_name);
    if (it != state.end()) {
        if (it->second == DepState::VISITING) {
            // Mutually-dependent runtime libraries (e.g. libc6 <-> libgcc-s1)
            // are common in real distro package graphs and aren't an install
            // -order problem here -- wish just extracts files, it doesn't run
            // maintainer scripts that need a strict order. Treat this as
            // "already being handled by an ancestor call" rather than a hard
            // failure; the ancestor will still add pkg_name to `resolved`
            // once its own dependency loop finishes.
            return Result<std::vector<std::string>>::success(resolved);
        }
        if (it->second == DepState::VISITED) {
            return Result<std::vector<std::string>>::success(resolved);
        }
    }

    state[pkg_name] = DepState::VISITING;

    auto pkg_r = index_->get(pkg_name);
    if (!pkg_r) {
        // Used to warn and tolerate this (the mirror's catalog has real,
        // known gaps) -- promoted to a hard failure: silently installing a
        // package while skipping something it explicitly declared it
        // needs is worse than refusing outright and saying why.
        state[pkg_name] = DepState::VISITED;
        return Result<std::vector<std::string>>::failure(pkg_r.error());
    }

    // Conflicts: does this package collide with anything else this same
    // install transaction already needs? (Conflicts with packages already
    // INSTALLED on the system are checked separately by PackageManager,
    // which has the installed-package database this resolver doesn't.)
    for (const auto& c : pkg_r.value().conflicts) {
        for (const auto& alt : c.alternatives) {
            if (std::find(resolved.begin(), resolved.end(), alt.name) != resolved.end()) {
                return Result<std::vector<std::string>>::failure(
                    pkg_name + " conflicts with " + alt.name + ", which this install already needs");
            }
        }
    }
    for (const auto& b : pkg_r.value().breaks) {
        for (const auto& alt : b.alternatives) {
            if (std::find(resolved.begin(), resolved.end(), alt.name) != resolved.end()) {
                return Result<std::vector<std::string>>::failure(
                    pkg_name + " breaks " + alt.name + ", which this install already needs");
            }
        }
    }

    for (const auto& dep : pkg_r.value().depends) {
        auto result = resolve_dep_spec(dep, state, resolved);
        if (!result) return result;
    }

    state[pkg_name] = DepState::VISITED;
    resolved.push_back(pkg_name);
    return Result<std::vector<std::string>>::success(resolved);
}

inline Result<std::vector<std::string>> DependencyResolver::resolve_dep_spec(
    const DepSpec& dep,
    std::map<std::string, DepState>& state,
    std::vector<std::string>& resolved)
{
    bool found_but_wrong_version = false;
    std::string version_error;

    for (const auto& alt : dep.alternatives) {
        std::string chosen_name;

        // Name match first -- a literal package always shadows a Provides
        // entry with the same name. Constraint is applied to WHICHEVER
        // candidate the name matched against, after the name resolves, not
        // before: the constraint means nothing until we know which real
        // package's version it's being checked against.
        auto pkg_r = index_->get(alt.name);
        if (pkg_r) {
            if (!alt.constraint.satisfied_by(pkg_r.value().version)) {
                found_but_wrong_version = true;
                version_error = alt.name + " " + pkg_r.value().version +
                                 " does not satisfy required" + alt.constraint.to_string();
                continue; // this alternative is unusable, try the next one
            }
            chosen_name = alt.name;
        } else {
            // Not a real package name -- maybe it's a virtual name
            // (Provides:). Each candidate provider is checked against the
            // SAME constraint (applied post-match, same as above) using
            // the provider's own real version -- a provider whose version
            // doesn't satisfy it is skipped in favor of the next provider,
            // not silently accepted.
            for (const auto& provider : index_->get_providers(alt.name)) {
                auto prov_r = index_->get(provider);
                if (!prov_r) continue;
                if (!alt.constraint.satisfied_by(prov_r.value().version)) {
                    found_but_wrong_version = true;
                    version_error = provider + " " + prov_r.value().version +
                                     " (providing " + alt.name + ") does not satisfy required" +
                                     alt.constraint.to_string();
                    continue;
                }
                chosen_name = provider;
                break;
            }
        }

        if (chosen_name.empty()) continue; // this alternative doesn't exist anywhere -- try the next

        auto sub = resolve_recursive(chosen_name, state, resolved);
        if (!sub) return sub; // hard failure (conflict/breaks/constraint) propagates up
        return Result<std::vector<std::string>>::success(resolved);
    }

    if (found_but_wrong_version) {
        return Result<std::vector<std::string>>::failure(version_error);
    }
    // Nothing in this OR-group exists anywhere, directly or via provides.
    // Used to warn and continue past this (the mirror's catalog has real,
    // known gaps); promoted to a hard failure for the same reason a direct
    // "not found" now is -- see resolve_recursive.
    return Result<std::vector<std::string>>::failure(
        "Missing dependency: " + (dep.alternatives.empty() ? std::string("?") : dep.primary_name()));
}

inline Result<void> DependencyResolver::show_graph(const std::string& pkg_name, int depth,
                                                    std::set<std::string>& ancestors) {
    auto pkg_r = index_->get(pkg_name);
    if (!pkg_r) {
        return Result<void>::failure("Package not found: " + pkg_name);
    }

    // Real dependency graphs in this catalog contain cycles (e.g. libc6 <->
    // libgcc-s1) -- recursing without tracking the current path never
    // terminates. `ancestors` holds only the current root-to-here chain (not
    // everything ever seen), so a diamond re-visit -- same package reached
    // via two different non-cyclic branches -- still prints normally; only
    // an actual cycle back onto the current path gets cut short.
    if (ancestors.count(pkg_name)) {
        std::cout << std::string(depth * 2, ' ') << pkg_name << " (circular, see above)\n";
        return Result<void>::success();
    }
    ancestors.insert(pkg_name);

    std::cout << std::string(depth * 2, ' ') << pkg_name;
    std::cout << " (" << pkg_r.value().version << ")\n";

    for (const auto& dep : pkg_r.value().depends) {
        // Mirrors resolve_dep_spec's own selection logic (name match +
        // constraint first, then provides + constraint) read-only, purely
        // for display -- shows the alternative/provider that would ACTUALLY
        // be picked, not just always the first-listed one, since those can
        // differ (a version-mismatched first alternative correctly falls
        // through to a later one, or resolves via a virtual provider).
        std::string chosen_name, via_note;
        for (const auto& alt : dep.alternatives) {
            auto alt_r = index_->get(alt.name);
            if (alt_r && alt.constraint.satisfied_by(alt_r.value().version)) {
                chosen_name = alt.name;
                break;
            }
            for (const auto& provider : index_->get_providers(alt.name)) {
                auto prov_r = index_->get(provider);
                if (prov_r && alt.constraint.satisfied_by(prov_r.value().version)) {
                    chosen_name = provider;
                    via_note = " (via " + alt.name + ")";
                    break;
                }
            }
            if (!chosen_name.empty()) break;
        }
        if (chosen_name.empty()) {
            std::cout << std::string((depth + 1) * 2, ' ') << "! " << dep.to_string()
                      << " (unresolved)\n";
            continue;
        }
        if (!via_note.empty()) {
            std::cout << std::string((depth + 1) * 2, ' ') << via_note.substr(1) << ":\n";
        }
        show_graph(chosen_name, depth + 1, ancestors);
    }
    ancestors.erase(pkg_name);
    return Result<void>::success();
}

// ============================================================
// Service Manager (daemon supervision)
// ============================================================

// Service definitions live in <services_dir>/<name>.json; runtime PID files in
// <run_dir>/<name>.pid. start() double-detaches the daemon (setsid), redirects
// its stdio to a log file, and records the child PID so status/stop can find
// it later.
//
// Two ways enabled services actually run:
//   - start_enabled(): one-shot, dependency-ordered launch with no ongoing
//     supervision -- used when `wish init` is run manually/for testing, not
//     as the real PID 1.
//   - supervise(): the real init event loop (see below), used when `wish`
//     genuinely IS PID 1. Starts everything in dependency order, then
//     reaps zombies, restarts crashed services per restart_policy
//     ("always"/"on-failure"/"no"), and on SIGTERM/SIGINT stops everything
//     in reverse dependency order before returning.
class ServiceManager {
    std::string svc_dir_;
    std::string run_dir_;

    std::string def_path(const std::string& name) const { return svc_dir_ + "/" + name + ".json"; }
    std::string pid_path(const std::string& name) const { return run_dir_ + "/" + name + ".pid"; }

    Result<ServiceDef> load(const std::string& name) {
        std::string p = def_path(name);
        if (!fs::exists(p)) return Result<ServiceDef>::failure("Service not defined: " + name);
        try {
            std::ifstream f(p);
            json j;
            f >> j;
            return Result<ServiceDef>::success(ServiceDef::from_json(j));
        } catch (...) {
            return Result<ServiceDef>::failure("Corrupt service definition: " + name);
        }
    }

    Result<void> store(const ServiceDef& s) {
        std::error_code ec;
        fs::create_directories(svc_dir_, ec);
        if (!write_file_atomic(def_path(s.name), s.to_json().dump(2)))
            return Result<void>::failure("Cannot write service definition");
        return Result<void>::success();
    }

    pid_t read_pid(const std::string& name) {
        std::ifstream f(pid_path(name));
        pid_t pid = -1;
        if (f >> pid) return pid;
        return -1;
    }

    bool is_alive(pid_t pid) {
        if (pid <= 0) return false;
        return (kill(pid, 0) == 0) || (errno == EPERM);
    }

public:
    ServiceManager()
        : svc_dir_(Config::get_services_dir()),
          run_dir_(Config::get_run_dir()) {}

    Result<void> initialize() {
        std::error_code ec;
        fs::create_directories(svc_dir_, ec);
        fs::create_directories(run_dir_, ec);
        return Result<void>::success();
    }

    Result<void> define(const std::string& name, const std::string& command,
                        const std::string& restart_policy, bool enabled) {
        if (!PathValidator::is_safe_package_name(name))
            return Result<void>::failure("Invalid service name (use lowercase letters, digits, '-')");
        if (command.empty())
            return Result<void>::failure("Service command required");

        ServiceDef s;
        s.name = name;
        s.command = command;
        s.restart_policy = restart_policy.empty() ? "no" : restart_policy;
        s.enabled = enabled;
        s.log_file = "/var/log/wish/" + name + ".log";

        auto r = store(s);
        if (!r) return r;
        Logger::ok("Service defined: " + name);
        return Result<void>::success();
    }

    // Shared fork+setsid+exec logic behind both the one-shot `start()` (CLI
    // path) and `supervise()` (PID-1 event loop, which also needs it for
    // restarts). Does not check "already running" -- callers own that.
    Result<pid_t> launch(const ServiceDef& s) {
        std::error_code ec;
        fs::create_directories(run_dir_, ec);
        fs::path logdir = fs::path(s.log_file).parent_path();
        if (!logdir.empty()) fs::create_directories(logdir, ec);

        pid_t pid = fork();
        if (pid < 0) return Result<pid_t>::failure("fork() failed");

        if (pid == 0) {
            setsid();
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
            int logfd = open(s.log_file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (logfd >= 0) { dup2(logfd, STDOUT_FILENO); dup2(logfd, STDERR_FILENO); close(logfd); }
            execl("/bin/sh", "sh", "-c", s.command.c_str(), (char*)nullptr);
            _exit(127);
        }

        write_file_atomic(pid_path(s.name), std::to_string(pid) + "\n");
        return Result<pid_t>::success(pid);
    }

    // Dependency-ordered enabled service list (DFS, 3-state cycle-tolerant
    // marking -- same shape as DependencyResolver::resolve_recursive: a
    // cycle isn't a hard failure, the ancestor call just finishes ordering
    // it). A dependency naming a service that doesn't exist or isn't
    // enabled is skipped with a warning rather than pulled in implicitly --
    // an enabled service's dependencies must themselves be explicitly
    // enabled.
    enum class VisitState { UNVISITED, VISITING, VISITED };

    void topo_visit(const std::string& name,
                     const std::map<std::string, ServiceDef>& all,
                     std::map<std::string, VisitState>& state,
                     std::vector<ServiceDef>& order) {
        auto it = state.find(name);
        if (it != state.end() && it->second != VisitState::UNVISITED) return;
        state[name] = VisitState::VISITING;

        auto def_it = all.find(name);
        if (def_it == all.end() || !def_it->second.enabled) {
            state[name] = VisitState::VISITED;
            return;
        }
        for (const auto& dep : def_it->second.dependencies) {
            if (all.find(dep) == all.end() || !all.at(dep).enabled) {
                Logger::warn("Service " + name + " depends on " + dep +
                             ", which is not defined/enabled -- starting without it");
                continue;
            }
            topo_visit(dep, all, state, order);
        }
        state[name] = VisitState::VISITED;
        order.push_back(def_it->second);
    }

    std::vector<ServiceDef> topo_order_enabled() {
        std::map<std::string, ServiceDef> all;
        std::error_code ec;
        if (fs::exists(svc_dir_)) {
            for (const auto& p : fs::directory_iterator(svc_dir_, ec)) {
                if (p.path().extension() != ".json") continue;
                auto sd = load(p.path().stem().string());
                if (sd) all[sd.value().name] = sd.value();
            }
        }
        std::map<std::string, VisitState> state;
        std::vector<ServiceDef> order;
        for (const auto& [name, def] : all) {
            if (def.enabled) topo_visit(name, all, state, order);
        }
        return order;
    }

    Result<void> start(const std::string& name) {
        auto root = Security::require_root("Service start");
        if (!root) return root;

        auto sd = load(name);
        if (!sd) return Result<void>::failure(sd.error());
        ServiceDef s = sd.value();

        pid_t existing = read_pid(name);
        if (is_alive(existing))
            return Result<void>::failure("Service already running (pid " + std::to_string(existing) + ")");

        auto pid_r = launch(s);
        if (!pid_r) return Result<void>::failure(pid_r.error());
        Logger::ok("Started " + name + " (pid " + std::to_string(pid_r.value()) + ")");
        return Result<void>::success();
    }

    Result<void> stop(const std::string& name) {
        auto root = Security::require_root("Service stop");
        if (!root) return root;

        pid_t pid = read_pid(name);
        if (!is_alive(pid)) {
            std::error_code ec;
            fs::remove(pid_path(name), ec);
            return Result<void>::failure("Service not running: " + name);
        }

        kill(pid, SIGTERM);
        for (int i = 0; i < 20 && is_alive(pid); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (is_alive(pid)) {
            Logger::warn("Service did not stop gracefully, sending SIGKILL");
            kill(pid, SIGKILL);
        }

        std::error_code ec;
        fs::remove(pid_path(name), ec);
        Logger::ok("Stopped " + name);
        return Result<void>::success();
    }

    Result<void> status(const std::string& name) {
        auto sd = load(name);
        if (!sd) return Result<void>::failure(sd.error());
        ServiceDef s = sd.value();
        pid_t pid = read_pid(name);
        bool alive = is_alive(pid);

        std::cout << "Service: " << s.name << "\n";
        std::cout << "  command: " << s.command << "\n";
        std::cout << "  enabled: " << (s.enabled ? "yes" : "no") << "\n";
        std::cout << "  restart: " << s.restart_policy << "\n";
        std::cout << "  status:  "
                  << (alive ? "running (pid " + std::to_string(pid) + ")" : "stopped") << "\n";
        std::cout << "  log:     " << s.log_file << "\n";
        return Result<void>::success();
    }

    Result<void> set_enabled(const std::string& name, bool en) {
        auto root = Security::require_root("Service enable/disable");
        if (!root) return root;

        auto sd = load(name);
        if (!sd) return Result<void>::failure(sd.error());
        ServiceDef s = sd.value();
        s.enabled = en;
        auto r = store(s);
        if (!r) return r;
        Logger::ok(std::string(en ? "Enabled " : "Disabled ") + name);
        return Result<void>::success();
    }

    Result<void> list() {
        std::error_code ec;
        if (!fs::exists(svc_dir_)) {
            std::cout << "No services defined.\n";
            return Result<void>::success();
        }
        bool any = false;
        std::cout << "Services:\n";
        std::cout << std::string(50, '=') << "\n";
        for (const auto& p : fs::directory_iterator(svc_dir_, ec)) {
            if (p.path().extension() != ".json") continue;
            std::string name = p.path().stem().string();
            auto sd = load(name);
            if (!sd) continue;
            ServiceDef s = sd.value();
            pid_t pid = read_pid(name);
            bool alive = is_alive(pid);
            std::cout << (alive ? " [running] " : " [stopped] ")
                      << name << (s.enabled ? " (enabled)" : "") << "\n";
            any = true;
        }
        if (!any) std::cout << "No services defined.\n";
        return Result<void>::success();
    }

    // Called by `wish init` when NOT running as PID 1 (e.g. manual/test
    // invocation): one-shot launch of every enabled service, in dependency
    // order, then returns. No crash monitoring -- that only happens under
    // supervise() below, which is what actually runs as the real init.
    void start_enabled() {
        for (const auto& def : topo_order_enabled()) {
            Logger::info("Starting enabled service: " + def.name);
            start(def.name);
        }
    }

    // The real PID-1 event loop: starts every enabled service in dependency
    // order, then blocks handling two things until told to shut down --
    //   - SIGCHLD: reap every exited child via a WNOHANG waitpid loop, not
    //     just the ones we're supervising. As PID 1, any orphaned process
    //     whose original parent died gets reparented to us and MUST be
    //     reaped here or it stays a zombie forever -- this loop does that
    //     for free just by draining waitpid(-1, ...) to exhaustion. A
    //     tracked service that crashed or exited gets restarted according
    //     to its restart_policy ("always" or "on-failure"), with a backoff
    //     that grows per consecutive restart and gives up after 5 in a row
    //     (a crash-looping service would otherwise pin the CPU forever).
    //   - SIGTERM/SIGINT: graceful shutdown -- stop every running service
    //     in REVERSE dependency order (so nothing gets torn down out from
    //     under something that depends on it), SIGTERM first, escalating
    //     to SIGKILL for anything still alive after a grace period, then
    //     return once everything supervised has exited.
    void supervise() {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGCHLD);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGINT);
        sigprocmask(SIG_BLOCK, &mask, nullptr);
        // SFD_NONBLOCK is essential here: the inner drain loop below calls
        // read() until it comes up empty to make sure it processes every
        // queued signal before going back to poll(), but a blocking
        // signalfd would hang forever on that final read once the queue is
        // actually empty instead of returning EAGAIN.
        int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
        if (sfd < 0) {
            Logger::error("supervise: signalfd() failed, cannot run as init event loop");
            return;
        }

        std::vector<ServiceDef> order = topo_order_enabled();
        std::map<pid_t, ServiceDef> running;
        std::map<std::string, int> restart_count;

        for (const auto& def : order) {
            auto pid_r = launch(def);
            if (pid_r) {
                running[pid_r.value()] = def;
                Logger::ok("Started " + def.name + " (pid " + std::to_string(pid_r.value()) + ")");
            } else {
                Logger::error("Failed to start " + def.name + ": " + pid_r.error());
            }
        }

        bool shutting_down = false;
        auto shutdown_deadline = std::chrono::steady_clock::time_point::max();
        const auto GRACE_PERIOD = std::chrono::seconds(10);

        auto begin_shutdown = [&]() {
            if (shutting_down) return;
            shutting_down = true;
            shutdown_deadline = std::chrono::steady_clock::now() + GRACE_PERIOD;
            Logger::info("Shutting down services (reverse dependency order)...");
            for (auto it = order.rbegin(); it != order.rend(); ++it) {
                for (const auto& [pid, def] : running) {
                    if (def.name == it->name) kill(pid, SIGTERM);
                }
            }
        };

        struct pollfd pfd = { sfd, POLLIN, 0 };
        while (!(shutting_down && running.empty())) {
            int timeout_ms = 1000;
            if (shutting_down) {
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    shutdown_deadline - std::chrono::steady_clock::now()).count();
                timeout_ms = static_cast<int>(std::max<long long>(0, std::min<long long>(remaining, 1000)));
            }

            int n = poll(&pfd, 1, timeout_ms);
            if (n > 0 && (pfd.revents & POLLIN)) {
                struct signalfd_siginfo si;
                while (read(sfd, &si, sizeof(si)) == sizeof(si)) {
                    if (si.ssi_signo == SIGCHLD) {
                        int status;
                        pid_t p;
                        while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
                            auto it = running.find(p);
                            if (it == running.end()) continue; // reparented orphan -- just reaping it is enough
                            ServiceDef def = it->second;
                            running.erase(it);
                            std::error_code ec;
                            fs::remove(pid_path(def.name), ec);

                            bool crashed = !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
                            if (shutting_down) {
                                // A service dying from the SIGTERM we just sent it
                                // looks identical to a crash (WIFEXITED is false
                                // either way) -- don't call it one.
                                Logger::ok(def.name + " stopped");
                                continue;
                            }
                            Logger::warn(def.name + (crashed ? " crashed" : " exited"));

                            bool should_restart =
                                def.restart_policy == "always" ||
                                (def.restart_policy == "on-failure" && crashed);
                            if (!should_restart) {
                                restart_count.erase(def.name);
                                continue;
                            }

                            int& rc = restart_count[def.name];
                            if (rc >= 5) {
                                Logger::error(def.name + ": crash-looping, giving up after 5 restarts");
                                continue;
                            }
                            rc++;
                            std::this_thread::sleep_for(std::chrono::milliseconds(500 * rc));
                            auto pid_r = launch(def);
                            if (pid_r) {
                                running[pid_r.value()] = def;
                                Logger::ok("Restarted " + def.name + " (pid " +
                                           std::to_string(pid_r.value()) + ", attempt " +
                                           std::to_string(rc) + ")");
                            } else {
                                Logger::error("Restart of " + def.name + " failed: " + pid_r.error());
                            }
                        }
                    } else if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) {
                        begin_shutdown();
                    }
                }
            }

            if (shutting_down && std::chrono::steady_clock::now() >= shutdown_deadline) {
                for (const auto& [pid, def] : running) {
                    Logger::warn(def.name + " did not stop gracefully, sending SIGKILL");
                    kill(pid, SIGKILL);
                }
                shutdown_deadline = std::chrono::steady_clock::time_point::max();
            }
        }
        close(sfd);
        Logger::ok("All services stopped");
    }
};

// ============================================================
// Process execution helpers (fork+execvp, no shell -- every argv element is
// passed straight to the child, so nothing here is interpretable as a shell
// metacharacter even if a caller's string happens to contain one)
// ============================================================

namespace Proc {
    inline Result<void> run(const std::vector<std::string>& argv) {
        if (argv.empty()) return Result<void>::failure("empty command");
        std::vector<const char*> c_argv;
        for (const auto& a : argv) c_argv.push_back(a.c_str());
        c_argv.push_back(nullptr);

        pid_t pid = fork();
        if (pid < 0) return Result<void>::failure("fork() failed");
        if (pid == 0) {
            execvp(c_argv[0], const_cast<char* const*>(c_argv.data()));
            _exit(127);
        }
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return Result<void>::success();
        std::string cmd;
        for (const auto& a : argv) cmd += a + " ";
        return Result<void>::failure("command failed: " + cmd);
    }

    // Writes `input` to the child's stdin, then closes it and waits.
    inline Result<void> run_with_stdin(const std::vector<std::string>& argv, const std::string& input) {
        if (argv.empty()) return Result<void>::failure("empty command");
        int pipefd[2];
        if (pipe(pipefd) != 0) return Result<void>::failure("pipe() failed");
        std::vector<const char*> c_argv;
        for (const auto& a : argv) c_argv.push_back(a.c_str());
        c_argv.push_back(nullptr);

        pid_t pid = fork();
        if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return Result<void>::failure("fork() failed"); }
        if (pid == 0) {
            dup2(pipefd[0], STDIN_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
            execvp(c_argv[0], const_cast<char* const*>(c_argv.data()));
            _exit(127);
        }
        close(pipefd[0]);
        ssize_t written = write(pipefd[1], input.data(), input.size());
        (void)written;
        close(pipefd[1]);
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return Result<void>::success();
        return Result<void>::failure("command failed (with piped stdin): " + argv[0]);
    }

    // Captures the child's stdout as a string.
    inline Result<std::string> capture(const std::vector<std::string>& argv) {
        if (argv.empty()) return Result<std::string>::failure("empty command");
        int pipefd[2];
        if (pipe(pipefd) != 0) return Result<std::string>::failure("pipe() failed");
        std::vector<const char*> c_argv;
        for (const auto& a : argv) c_argv.push_back(a.c_str());
        c_argv.push_back(nullptr);

        pid_t pid = fork();
        if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return Result<std::string>::failure("fork() failed"); }
        if (pid == 0) {
            dup2(pipefd[1], STDOUT_FILENO);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
            close(pipefd[0]);
            close(pipefd[1]);
            execvp(c_argv[0], const_cast<char* const*>(c_argv.data()));
            _exit(127);
        }
        close(pipefd[1]);
        std::string out;
        char buf[4096];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
        close(pipefd[0]);
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return Result<std::string>::success(out);
        return Result<std::string>::failure("command failed (capture): " + argv[0]);
    }
}

// ============================================================
// Disk Installer (wish --build-tool / wish --complete-install)
// ============================================================

struct DiskInfo {
    std::string dev;
    uint64_t size_bytes;
};

class Installer {
    static std::string read_password(const std::string& prompt) {
        std::cout << prompt << std::flush;
        termios oldt{}, newt{};
        bool have_tty = (tcgetattr(STDIN_FILENO, &oldt) == 0);
        if (have_tty) {
            newt = oldt;
            newt.c_lflag &= ~(tcflag_t)ECHO;
            tcsetattr(STDIN_FILENO, TCSANOW, &newt);
        }
        std::string pw;
        std::getline(std::cin, pw);
        if (have_tty) {
            tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            std::cout << "\n";
        }
        return pw;
    }

    static std::string prompt_line(const std::string& prompt) {
        std::cout << prompt << std::flush;
        std::string line;
        std::getline(std::cin, line);
        return line;
    }

    static std::vector<DiskInfo> list_disks() {
        std::vector<DiskInfo> disks;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator("/sys/block", ec)) {
            std::string name = entry.path().filename().string();
            if (name.rfind("loop", 0) == 0 || name.rfind("ram", 0) == 0 || name.rfind("sr", 0) == 0)
                continue;
            std::ifstream sz(entry.path() / "size");
            uint64_t sectors = 0;
            if (sz >> sectors && sectors > 0) {
                disks.push_back({"/dev/" + name, sectors * 512});
            }
        }
        std::sort(disks.begin(), disks.end(), [](const DiskInfo& a, const DiskInfo& b) { return a.dev < b.dev; });
        return disks;
    }

    // nvme0n1 -> nvme0n1p1 ; sda -> sda1
    static std::string partition_path(const std::string& disk, int n) {
        if (!disk.empty() && isdigit((unsigned char)disk.back()))
            return disk + "p" + std::to_string(n);
        return disk + std::to_string(n);
    }

    static std::string human_size(uint64_t bytes) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << (bytes / 1e9) << " GB";
        return ss.str();
    }

    static Result<void> require_tools(std::initializer_list<const char*> tools) {
        for (const char* tool : tools) {
            if (!Proc::capture({"which", tool})) {
                return Result<void>::failure(std::string("Required tool not found: ") + tool +
                                             " (run 'wish install " + tool + "' first)");
            }
        }
        return Result<void>::success();
    }

    static void unmount_all(const std::string& target) {
        for (const char* p : {"dev", "sys", "proc"}) Proc::run({"umount", target + "/" + p});
        Proc::run({"umount", target + "/boot/efi"});
        Proc::run({"umount", target});
    }

public:
    // wish --build-tool: minimal disk installer (x86_64 only -- ARM targets
    // ship as a prebuilt .img instead, since most ARM boards are flashed
    // directly rather than booted into an interactive installer).
    Result<void> build_tool() {
        if (Config::get_arch() != "x86_64") {
            return Result<void>::failure(
                "wish --build-tool only supports x86_64. ARM targets use a prebuilt .img image instead.");
        }
        auto root = Security::require_root("Disk installation");
        if (!root) return root;

        auto tools = require_tools({"parted", "mkfs.fat", "mkfs.ext4", "grub-install", "blkid", "chroot"});
        if (!tools) return tools;

        auto disks = list_disks();
        if (disks.empty()) return Result<void>::failure("No disks found");

        std::cout << "\nwish build-tool -- minimal disk installer\n";
        std::cout << "===========================================\n\n";
        std::cout << "Available disks:\n";
        for (size_t i = 0; i < disks.size(); i++) {
            std::cout << "  [" << i << "] " << disks[i].dev << "  (" << human_size(disks[i].size_bytes) << ")\n";
        }

        int idx = -1;
        try { idx = std::stoi(prompt_line("\nSelect disk number: ")); } catch (...) {}
        if (idx < 0 || idx >= (int)disks.size()) return Result<void>::failure("Invalid selection");
        std::string disk = disks[idx].dev;

        std::string confirm = prompt_line("\nThis will ERASE ALL DATA on " + disk + ". Type YES to continue: ");
        if (confirm != "YES") return Result<void>::failure("Aborted");

        std::string root_pw = read_password("\nRoot password: ");
        std::string root_pw2 = read_password("Confirm root password: ");
        if (root_pw.empty() || root_pw != root_pw2) return Result<void>::failure("Passwords did not match or were empty");

        Logger::info("Partitioning " + disk + "...");
        auto r = Proc::run({"parted", "-s", disk, "mklabel", "gpt"});                              if (!r) return r;
        r = Proc::run({"parted", "-s", disk, "mkpart", "ESP", "fat32", "1MiB", "513MiB"});          if (!r) return r;
        r = Proc::run({"parted", "-s", disk, "set", "1", "esp", "on"});                             if (!r) return r;
        r = Proc::run({"parted", "-s", disk, "mkpart", "root", "ext4", "513MiB", "100%"});          if (!r) return r;

        std::string esp = partition_path(disk, 1);
        std::string rootp = partition_path(disk, 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(800)); // let the kernel re-read the partition table

        Logger::info("Formatting...");
        r = Proc::run({"mkfs.fat", "-F32", esp});    if (!r) return r;
        r = Proc::run({"mkfs.ext4", "-F", rootp});   if (!r) return r;

        Logger::info("Mounting target...");
        std::string target = "/mnt/wish-install";
        fs::create_directories(target);
        r = Proc::run({"mount", rootp, target});
        if (!r) return r;
        fs::create_directories(target + "/boot/efi");
        r = Proc::run({"mount", esp, target + "/boot/efi"});
        if (!r) { Proc::run({"umount", target}); return r; }

        Logger::info("Copying system files (this takes a while)...");
        r = Proc::run({"sh", "-c",
            "tar -C / --exclude=./proc --exclude=./sys --exclude=./dev --exclude=./run "
            "--exclude=./tmp --exclude=./mnt -cf - . | tar -C " + target + " -xpf -"});
        if (!r) { unmount_all(target); return Result<void>::failure("Failed to copy system files"); }
        for (const char* d : {"proc", "sys", "dev", "run", "tmp"}) fs::create_directories(target + "/" + d);

        Logger::info("Writing fstab...");
        auto uuid_root = Proc::capture({"blkid", "-s", "UUID", "-o", "value", rootp});
        auto uuid_esp  = Proc::capture({"blkid", "-s", "UUID", "-o", "value", esp});
        if (uuid_root && uuid_esp) {
            auto trim = [](std::string s) { while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); return s; };
            std::ofstream fstab(target + "/etc/fstab");
            fstab << "UUID=" << trim(uuid_root.value()) << "  /          ext4  defaults  0 1\n";
            fstab << "UUID=" << trim(uuid_esp.value())  << "  /boot/efi  vfat  defaults  0 2\n";
        } else {
            Logger::warn("Could not determine partition UUIDs; /etc/fstab not written");
        }

        Logger::info("Setting root password...");
        r = Proc::run_with_stdin({"chroot", target, "chpasswd"}, "root:" + root_pw + "\n");
        if (!r) Logger::warn("Failed to set root password: " + r.error());

        Logger::info("Installing bootloader...");
        for (const char* p : {"proc", "sys", "dev"}) {
            fs::create_directories(target + std::string("/") + p);
            Proc::run({"mount", "--bind", std::string("/") + p, target + "/" + p});
        }
        r = Proc::run({"chroot", target, "grub-install", "--target=x86_64-efi",
                       "--efi-directory=/boot/efi", "--bootloader-id=WishOS", "--removable"});
        if (!r) Logger::warn("grub-install failed: " + r.error());
        r = Proc::run({"chroot", target, "grub-mkconfig", "-o", "/boot/grub/grub.cfg"});
        if (!r) Logger::warn("grub-mkconfig failed (you may need to write /boot/grub/grub.cfg by hand): " + r.error());

        Logger::info("Cleaning up...");
        unmount_all(target);

        Logger::ok("Installation complete. Remove the install media and reboot.");
        Logger::info("After first boot, run 'wish --complete-install' to create a user account.");
        return Result<void>::success();
    }

    // wish --complete-install: post-install personalization -- create a
    // regular user, set their password, optionally grant sudo. Meant to be
    // run once, on the newly installed system itself (not chrooted).
    Result<void> complete_install() {
        auto root = Security::require_root("Complete install");
        if (!root) return root;

        std::cout << "\nwish complete-install -- account setup\n";
        std::cout << "========================================\n\n";

        std::string username = prompt_line("Username: ");
        if (username.empty() || !std::regex_match(username, std::regex("^[a-z_][a-z0-9_-]*$"))) {
            return Result<void>::failure("Invalid username (must start with a lowercase letter or '_', then lowercase letters/digits/-/_)");
        }

        std::string pw1 = read_password("Password: ");
        std::string pw2 = read_password("Confirm password: ");
        if (pw1.empty() || pw1 != pw2) {
            return Result<void>::failure("Passwords did not match or were empty");
        }

        auto r = Proc::run({"adduser", "-D", "-s", "/bin/sh", username});
        if (!r) {
            // Non-busybox adduser (Debian-style) needs --disabled-password instead of -D.
            r = Proc::run({"adduser", "--disabled-password", "--gecos", "", username});
        }
        if (!r) return Result<void>::failure("Failed to create user '" + username + "': " + r.error());

        r = Proc::run_with_stdin({"chpasswd"}, username + ":" + pw1 + "\n");
        if (!r) return Result<void>::failure("User created, but failed to set password: " + r.error());

        std::string ans = prompt_line("Grant sudo access? [y/N]: ");
        if (!ans.empty() && (ans[0] == 'y' || ans[0] == 'Y')) {
            auto g = Proc::run({"addgroup", username, "sudo"});
            if (!g) g = Proc::run({"adduser", username, "sudo"}); // Debian-style syntax fallback
            if (!g) {
                Logger::warn("Could not add " + username + " to the 'sudo' group: " + g.error());
            } else {
                std::error_code ec;
                fs::create_directories("/etc/sudoers.d", ec);
                std::ofstream sudoers("/etc/sudoers.d/wish-sudo-group");
                sudoers << "%sudo ALL=(ALL:ALL) ALL\n";
                sudoers.close();
                fs::permissions("/etc/sudoers.d/wish-sudo-group", fs::perms::owner_read | fs::perms::owner_write, ec);
                Logger::ok("Granted sudo access to " + username);
            }
        }

        Logger::ok("User '" + username + "' is ready. You can now log in.");
        return Result<void>::success();
    }
};

// ============================================================
// Init Manager
// ============================================================

class InitManager {
public:
    Result<void> initialize_system() {
        auto root_check = Security::require_root("System initialization");
        if (!root_check) return root_check;
        
        Logger::info("Initializing WishOS system...");
        
        // Mount proc
        if (!fs::exists("/proc")) {
            fs::create_directories("/proc");
        }
        if (mount("proc", "/proc", "proc", 0, nullptr) != 0 && errno != EBUSY) {
            Logger::warn("Failed to mount /proc: " + std::string(strerror(errno)));
        }
        
        // Mount sys
        if (!fs::exists("/sys")) {
            fs::create_directories("/sys");
        }
        if (mount("sysfs", "/sys", "sysfs", 0, nullptr) != 0 && errno != EBUSY) {
            Logger::warn("Failed to mount /sys: " + std::string(strerror(errno)));
        }
        
        // Mount dev (devtmpfs if available, else tmpfs)
        if (!fs::exists("/dev")) {
            fs::create_directories("/dev");
        }
        if (mount("devtmpfs", "/dev", "devtmpfs", 0, nullptr) != 0) {
            if (mount("tmpfs", "/dev", "tmpfs", 0, "mode=0755") != 0 && errno != EBUSY) {
                Logger::warn("Failed to mount /dev");
            }
        }
        
        // Set hostname
        std::string hostname = "wishos";
        std::ifstream etc_hostname("/etc/hostname");
        if (etc_hostname.is_open()) {
            std::getline(etc_hostname, hostname);
            etc_hostname.close();
        }
        if (sethostname(hostname.c_str(), hostname.length()) != 0) {
            Logger::warn("Failed to set hostname");
        }
        
        // Create wish directories
        try {
            fs::create_directories(Config::get_lib_dir());
            fs::create_directories(Config::get_cache_dir());
            fs::create_directories(Config::get_lib_dir() + "/manifests");
            fs::create_directories(Config::get_lib_dir() + "/layers");
        } catch (const fs::filesystem_error& e) {
            return Result<void>::failure("Cannot create system directories");
        }
        
        // Initialize databases
        PackageDatabase db;
        db.initialize();

        HistoryManager hist;
        hist.initialize();

        LayerManager layers;
        layers.initialize();

        GenerationManager gens;
        gens.initialize();

        ServiceManager services;
        services.initialize();

        Logger::ok("System initialized");

        if (getpid() == 1) {
            // We ARE the init process -- become the real PID-1 event loop:
            // start every enabled service in dependency order, then block
            // reaping zombies / restarting crashed services / handling a
            // graceful shutdown signal until told to stop. Never returns
            // under normal operation.
            Logger::info("Running as PID 1 -- entering supervisor event loop");
            services.supervise();
        } else {
            // Manual/test invocation of `wish init`, not actually PID 1:
            // one-shot autostart only, no ongoing supervision.
            services.start_enabled();
        }

        return Result<void>::success();
    }
};

// ============================================================
// Peer Server (LAN Package Sharing)
// ============================================================

class PeerServer {
    int server_fd_ = -1;
    bool running_ = false;
    std::thread server_thread_;
    std::string cache_dir_;
    bool peers_scanned_ = false;
    std::vector<std::string> cached_peers_;

    void handle_client(int client_fd) {
        char buffer[1024] = {0};
        int valread = read(client_fd, buffer, sizeof(buffer) - 1);
        if (valread > 0) {
            std::string request(buffer, valread);

            bool bad_request = false;
            std::string pkg_name;

            if (request.find("GET /") == 0) {
                size_t space = request.find(' ', 5);
                if (space == std::string::npos) {
                    bad_request = true;
                } else {
                    pkg_name = request.substr(5, space - 5);
                }
            } else {
                bad_request = true;
            }

            // Untrusted input from the network: only a bare filename is ever
            // valid here (peer clients only ask for entries in our flat cache
            // dir), so reject anything containing "..", "/", or odd chars
            // before it ever touches the filesystem.
            fs::path safe_path;
            if (bad_request || !PathValidator::is_safe_filename(pkg_name) ||
                !PathValidator::resolve_within(cache_dir_, pkg_name, safe_path)) {
                const char* bad = "HTTP/1.1 400 Bad Request\r\n\r\n";
                send(client_fd, bad, strlen(bad), 0);
                close(client_fd);
                return;
            }

            if (fs::exists(safe_path) && fs::is_regular_file(safe_path)) {
                std::ifstream file(safe_path, std::ios::binary);
                std::string content((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());

                std::string response = "HTTP/1.1 200 OK\r\n";
                response += "Content-Length: " + std::to_string(content.size()) + "\r\n";
                response += "\r\n";
                response += content;

                send(client_fd, response.c_str(), response.size(), 0);
            } else {
                const char* not_found = "HTTP/1.1 404 Not Found\r\n\r\n";
                send(client_fd, not_found, strlen(not_found), 0);
            }
        }
        close(client_fd);
    }
    
    void run_server() {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ == -1) return;
        
        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(Config::PEER_PORT);
        
        if (bind(server_fd_, (struct sockaddr*)&address, sizeof(address)) < 0) {
            close(server_fd_);
            return;
        }
        
        if (listen(server_fd_, 3) < 0) {
            close(server_fd_);
            return;
        }
        
        Logger::ok("Peer server listening on port " + std::to_string(Config::PEER_PORT));
        
        while (running_) {
            struct sockaddr_in client_addr;
            socklen_t addrlen = sizeof(client_addr);
            int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &addrlen);
            
            if (client_fd >= 0) {
                std::thread client_thread(&PeerServer::handle_client, this, client_fd);
                client_thread.detach();
            }
        }
        
        close(server_fd_);
    }
    
public:
    PeerServer() : cache_dir_(Config::get_cache_dir()) {}
    
    Result<void> start() {
        if (running_) {
            return Result<void>::failure("Server already running");
        }
        
        running_ = true;
        server_thread_ = std::thread(&PeerServer::run_server, this);
        
        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        if (server_fd_ == -1) {
            running_ = false;
            if (server_thread_.joinable()) server_thread_.join();
            return Result<void>::failure("Failed to start peer server");
        }
        
        Logger::ok("Peer server started on port " + std::to_string(Config::PEER_PORT));
        return Result<void>::success();
    }
    
    Result<void> stop() {
        if (!running_) {
            return Result<void>::failure("Server not running");
        }
        
        running_ = false;
        if (server_fd_ != -1) {
            shutdown(server_fd_, SHUT_RDWR);
            close(server_fd_);
        }
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        
        Logger::ok("Peer server stopped");
        return Result<void>::success();
    }
    
    // A blocking connect() (or SO_RCVTIMEO, which doesn't bound connect() at
    // all) against a silently-dropping host can take seconds; doing that
    // one-at-a-time across a 3x254-address scan turns "scan the LAN" into a
    // multi-minute stall before every install. Fire off a whole batch of
    // non-blocking connects concurrently and reap them with a single poll()
    // sharing one deadline, so wall-clock cost is (batches * timeout), not
    // (hosts * timeout).
    static std::vector<std::string> scan_batch(const std::vector<std::string>& ips, int port, int timeout_ms) {
        std::vector<int> fds(ips.size(), -1);
        std::vector<struct pollfd> pfds;
        pfds.reserve(ips.size());

        for (size_t i = 0; i < ips.size(); i++) {
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) continue;
            int flags = fcntl(sock, F_GETFL, 0);
            fcntl(sock, F_SETFL, flags | O_NONBLOCK);

            struct sockaddr_in addr {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            inet_pton(AF_INET, ips[i].c_str(), &addr.sin_addr);

            int rc = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
            if (rc == 0 || errno == EINPROGRESS) {
                fds[i] = sock;
                pfds.push_back({sock, POLLOUT, 0});
            } else {
                close(sock);
            }
        }

        if (!pfds.empty()) {
            poll(pfds.data(), pfds.size(), timeout_ms);
        }

        std::vector<std::string> found;
        for (size_t i = 0; i < ips.size(); i++) {
            if (fds[i] < 0) continue;
            for (const auto& p : pfds) {
                if (p.fd != fds[i]) continue;
                if (p.revents & POLLOUT) {
                    int so_error = 0;
                    socklen_t len = sizeof(so_error);
                    if (getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 && so_error == 0) {
                        found.push_back(ips[i]);
                    }
                }
                break;
            }
            close(fds[i]);
        }
        return found;
    }

    Result<std::vector<std::string>> scan_peers() {
        std::vector<std::string> candidates;
        std::vector<std::string> subnets = {"192.168.1.", "192.168.0.", "10.0.0."};
        for (const auto& subnet : subnets) {
            for (int i = 1; i < 255; i++) {
                candidates.push_back(subnet + std::to_string(i));
            }
        }

        std::vector<std::string> peers;
        const size_t batch_size = 256;
        for (size_t start = 0; start < candidates.size(); start += batch_size) {
            size_t end = std::min(start + batch_size, candidates.size());
            std::vector<std::string> batch(candidates.begin() + start, candidates.begin() + end);
            auto found = scan_batch(batch, Config::PEER_PORT, 200);
            peers.insert(peers.end(), found.begin(), found.end());
        }

        return Result<std::vector<std::string>>::success(peers);
    }
    
    Result<bool> try_peer_download(const std::string& pkg_name, const std::string& dest) {
        // scan_peers() sweeps three full /24 subnets (~760 raw connect()s).
        // Even just once per process, that burst is enough on some NAT/
        // virtualized network stacks (notably Docker Desktop's vpnkit) to
        // degrade the conntrack table and blackhole the real download that
        // follows -- reproduced repeatedly while baking iwd/network-manager
        // into a container rootfs. LAN peer discovery is opt-in now (most
        // installs have no peer mesh anyway): set WISH_ENABLE_PEER_DISCOVERY
        // to any non-empty value to turn it back on.
        if (!std::getenv("WISH_ENABLE_PEER_DISCOVERY")) {
            return Result<bool>::success(false);
        }
        if (!peers_scanned_) {
            auto peers_result = scan_peers();
            cached_peers_ = peers_result ? peers_result.value() : std::vector<std::string>{};
            peers_scanned_ = true;
        }
        if (cached_peers_.empty()) {
            return Result<bool>::success(false); // No peers available
        }

        for (const auto& peer : cached_peers_) {
            std::string url = "http://" + peer + ":" + std::to_string(Config::PEER_PORT) + "/" + pkg_name;
            auto result = HttpClient::download(url, dest);
            if (result) {
                Logger::ok("Downloaded from peer: " + peer);
                return Result<bool>::success(true);
            }
        }
        
        return Result<bool>::success(false);
    }
};

// ============================================================
// Transaction (atomic install with staged progress + auto-rollback)
// ============================================================

// Wraps a mutating operation so a failure part-way through leaves nothing
// behind: every file extracted during the transaction is tracked, and the
// pre-transaction bytes of installed.json are held in memory. rollback()
// deletes the tracked files (reverse order) and restores the DB file, so a
// failed `wish install` never leaves half-written packages or a corrupt DB.
class Transaction {
    int id_ = 0;
    std::string db_file_;
    std::string db_backup_;
    bool db_existed_ = false;
    std::vector<std::string> created_files_;
    bool committed_ = false;

    static int next_id() {
        std::string cf = Config::get_lib_dir() + "/tx-counter";
        int n = 0;
        { std::ifstream f(cf); f >> n; }
        n++;
        write_file_atomic(cf, std::to_string(n));
        return n;
    }

public:
    Transaction() : db_file_(Config::get_lib_dir() + "/installed.json") {}

    void begin(const std::string& op, const std::string& pkg) {
        id_ = next_id();
        committed_ = false;
        created_files_.clear();
        db_existed_ = fs::exists(db_file_);
        if (db_existed_) {
            std::ifstream f(db_file_, std::ios::binary);
            db_backup_.assign((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
        }
        std::cout << "\nTransaction " << id_ << " (" << op << " " << pkg << ")\n";
    }

    void stage_ok(const std::string& name)   { std::cout << "[✓] " << name << "\n"; }
    void stage_fail(const std::string& name) { std::cout << "[✗] " << name << "\n"; }

    void track(const std::vector<std::string>& files) {
        created_files_.insert(created_files_.end(), files.begin(), files.end());
    }

    void commit() { committed_ = true; }

    void rollback(const std::string& reason) {
        if (committed_) return;
        Logger::warn("Rolling back transaction " + std::to_string(id_) + ": " + reason);
        for (auto it = created_files_.rbegin(); it != created_files_.rend(); ++it) {
            std::error_code ec;
            if (fs::exists(*it) && !fs::is_directory(*it)) fs::remove(*it, ec);
        }
        if (db_existed_) {
            write_file_atomic(db_file_, db_backup_);
        } else {
            std::error_code ec;
            fs::remove(db_file_, ec);
        }
        Logger::ok("Rolled back to previous state");
    }

    int id() const { return id_; }
};

// ============================================================
// Package Manager
// ============================================================

class PackageManager {
    RemoteIndex index_;
    PackageDatabase db_;
    HistoryManager history_;
    DependencyResolver resolver_;
    PeerServer peer_;
    GenerationManager gen_;
    Lockfile lock_;
    bool locked_ = false;

    // Idempotent within one PackageManager instance -- safe to call from
    // nested mutating operations (autoremove() -> remove(), upgrade() ->
    // install_impl()) without the second call deadlocking against the
    // first: flock() locks are scoped per open-file-description, so a
    // second Lockfile object opening the same path in the same process
    // would otherwise block against its own already-held lock.
    Result<void> ensure_locked() {
        if (locked_) return Result<void>::success();
        auto r = lock_.acquire();
        if (r) locked_ = true;
        return r;
    }

    Result<std::string> download_from_source(const PackageInfo& pkg) {
        std::string cache_dir = Config::get_cache_dir();
        std::string pkg_file = pkg.filename();
        std::string local_pkg = cache_dir + "/" + pkg_file;
        std::string local_sha = local_pkg + ".sha256";
        
        // Try peers first
        auto peer_result = peer_.try_peer_download(pkg_file, local_pkg);
        if (peer_result && peer_result.value()) {
            // Need to get SHA from somewhere
            std::string sha_url = Config::get_repo_url() + "/pkgs/" + Config::get_arch() + "/" + pkg_file + ".sha256";
            auto sha_result = HttpClient::get(sha_url);
            if (sha_result) {
                std::ofstream sha_file(local_sha);
                sha_file << sha_result.value();
            }
            return Result<std::string>::success(local_pkg);
        }
        
        // Fall back to repository
        std::string pkg_url = Config::get_repo_url() + "/pkgs/" + Config::get_arch() + "/" + pkg_file;
        std::string sha_url = pkg_url + ".sha256";
        
        // Download SHA256
        Logger::info("Fetching checksum...");
        auto sha_result = HttpClient::get(sha_url);
        if (!sha_result) {
            return Result<std::string>::failure("Failed to download checksum: " + sha_result.error());
        }
        
        std::string expected_hash = sha_result.value();
        size_t space = expected_hash.find(' ');
        if (space != std::string::npos) {
            expected_hash = expected_hash.substr(0, space);
        }
        
        // Download package
        Logger::info("Fetching package...");
        auto dl_result = HttpClient::download(pkg_url, local_pkg);
        if (!dl_result) {
            return Result<std::string>::failure("Failed to download: " + dl_result.error());
        }
        
        // Verify
        auto verify_result = Crypto::verify_file(local_pkg, expected_hash);
        if (!verify_result || !verify_result.value()) {
            fs::remove(local_pkg);
            return Result<std::string>::failure("Checksum verification failed");
        }
        
        return Result<std::string>::success(local_pkg);
    }
    
    // Staged, transactional install. Any hard failure rolls back every file
    // written so far and restores the package DB, then a generation snapshot is
    // taken on commit. install() calls this after the "already installed"
    // check; upgrade() calls it directly (skipping that check on purpose,
    // otherwise upgrade would no-op).
    // The resolver's own conflicts/breaks checks (per-visit + final pass)
    // only ever look within the newly-resolved closure -- they have no
    // visibility into what's ALREADY installed, since DependencyResolver
    // only holds a RemoteIndex pointer. This is the missing other half,
    // checked here because PackageManager is the one place that has both
    // `index_` and `db_`. Bidirectional on purpose: conflict/breaks
    // declarations aren't guaranteed symmetric in real package data (only
    // one side of a pair might actually declare it), so both directions
    // are checked -- does anything about to be installed conflict with
    // something already on the system, AND does anything already on the
    // system conflict with something about to be installed.
    Result<void> check_conflicts_with_installed(const std::vector<std::string>& deps) {
        auto installed = db_.get_all();
        for (const auto& name : deps) {
            if (installed.count(name)) continue; // already compatible with the system by definition
            auto pkg_r = index_.get(name);
            if (!pkg_r) continue; // already resolved successfully, shouldn't happen
            const auto& pkg = pkg_r.value();

            for (const auto& c : pkg.conflicts) {
                for (const auto& alt : c.alternatives) {
                    if (installed.count(alt.name)) {
                        return Result<void>::failure(
                            name + " conflicts with installed package " + alt.name);
                    }
                }
            }
            for (const auto& b : pkg.breaks) {
                for (const auto& alt : b.alternatives) {
                    if (installed.count(alt.name)) {
                        return Result<void>::failure(
                            name + " breaks installed package " + alt.name);
                    }
                }
            }
            for (const auto& [iname, ipkg] : installed) {
                for (const auto& c : ipkg.conflicts) {
                    for (const auto& alt : c.alternatives) {
                        if (alt.name == name) {
                            return Result<void>::failure(
                                "installed package " + iname + " conflicts with " + name);
                        }
                    }
                }
                for (const auto& b : ipkg.breaks) {
                    for (const auto& alt : b.alternatives) {
                        if (alt.name == name) {
                            return Result<void>::failure(
                                "installed package " + iname + " breaks " + name);
                        }
                    }
                }
            }
        }
        return Result<void>::success();
    }

    Result<void> install_impl(const std::string& pkg_name, bool dry_run = false) {
        Logger::info((dry_run ? "Would install: " : "Installing: ") + pkg_name);

        // Ensure index is loaded
        if (index_.get_all().empty()) {
            auto update_result = update();
            if (!update_result) return update_result;
        }

        if (dry_run) {
            // No Transaction, no writes anywhere -- just resolve and report.
            auto deps_result = resolver_.resolve(pkg_name);
            if (!deps_result) {
                return Result<void>::failure("Dependency resolution failed: " + deps_result.error());
            }
            auto conflict_check = check_conflicts_with_installed(deps_result.value());
            if (!conflict_check) {
                return Result<void>::failure("Dependency resolution failed: " + conflict_check.error());
            }
            int new_count = 0;
            for (const auto& dep : deps_result.value()) {
                bool already = db_.is_installed(dep);
                std::cout << "  -> " << dep << (already ? " (already installed)" : " (new)") << "\n";
                if (!already) new_count++;
            }
            std::cout << new_count << " package(s) would be newly installed.\n";
            return Result<void>::success();
        }

        Transaction tx;
        tx.begin("install", pkg_name);

        // Stage 1: resolve dependencies
        auto deps_result = resolver_.resolve(pkg_name);
        if (!deps_result) {
            tx.stage_fail("Resolve dependencies");
            tx.rollback(deps_result.error());
            history_.record("install", pkg_name, {}, {}, false, deps_result.error());
            return Result<void>::failure("Dependency resolution failed: " + deps_result.error());
        }
        // The resolver only ever checks conflicts/breaks within the newly-
        // resolved closure -- it has no visibility into what's already
        // installed. This is the other half, bidirectional (see
        // check_conflicts_with_installed's own comment).
        auto conflict_check = check_conflicts_with_installed(deps_result.value());
        if (!conflict_check) {
            tx.stage_fail("Resolve dependencies");
            tx.rollback(conflict_check.error());
            history_.record("install", pkg_name, {}, {}, false, conflict_check.error());
            return Result<void>::failure("Dependency resolution failed: " + conflict_check.error());
        }
        tx.stage_ok("Resolve dependencies");

        auto deps = deps_result.value();
        for (const auto& dep : deps) {
            std::cout << "  -> " << dep << "\n";
        }

        // Stages 2-4: download + verify + install, in resolved order (main last).
        // A failure on any package aborts and rolls the whole transaction back.
        for (const auto& item : deps) {
            if (db_.is_installed(item)) continue;

            auto pkg_r = index_.get(item);
            if (!pkg_r) {
                tx.stage_fail("Download " + item);
                tx.rollback(pkg_r.error());
                history_.record("install", item, deps, {}, false, pkg_r.error());
                return Result<void>::failure("Package not found: " + item);
            }

            auto dl = download_from_source(pkg_r.value()); // verifies checksum internally
            if (!dl) {
                tx.stage_fail("Download " + item);
                tx.rollback(dl.error());
                history_.record("install", item, deps, {}, false, dl.error());
                return Result<void>::failure("Download failed: " + dl.error());
            }

            auto ex = ArchiveExtractor::extract(dl.value(), Config::get_root_dir());
            if (!ex) {
                tx.stage_fail("Install " + item);
                tx.rollback(ex.error());
                history_.record("install", item, deps, {}, false, ex.error());
                return Result<void>::failure("Extraction failed: " + ex.error());
            }

            tx.track(ex.value());

            PackageInfo installed_pkg = pkg_r.value();
            // Only the exact package the caller asked for is "explicit" --
            // everything else in this closure was pulled in solely to
            // satisfy a dependency, which is exactly what orphan detection/
            // autoremove needs to tell apart later.
            installed_pkg.install_reason = (item == pkg_name) ? "explicit" : "auto";
            std::error_code sz_ec;
            installed_pkg.installed_size = 0;
            for (const auto& f : ex.value()) {
                if (fs::is_regular_file(f, sz_ec)) installed_pkg.installed_size += fs::file_size(f, sz_ec);
            }
            installed_pkg.download_size = fs::exists(dl.value()) ? fs::file_size(dl.value(), sz_ec) : 0;

            db_.install(installed_pkg, ex.value());
            history_.record("install", item, {}, ex.value());
        }
        tx.stage_ok("Download");
        tx.stage_ok("Verify");
        tx.stage_ok("Install");

        // Stage 5: commit -> snapshot the new system state as a generation.
        tx.commit();
        auto snap = gen_.snapshot("install " + pkg_name, db_.get_all(),
                                  Config::get_root_dir(), Config::get_managed_config_paths());
        tx.stage_ok("Commit");
        if (snap) Logger::ok("Created generation " + std::to_string(snap.value()));

        Logger::ok(pkg_name + " installed successfully");
        return Result<void>::success();
    }

public:
    PackageManager() { resolver_.set_index(&index_); }

    Result<void> update() {
        auto result = index_.fetch();
        return result;
    }
    
    Result<void> install(const std::string& pkg_name, bool dry_run = false) {
        auto root_check = Security::require_root("Installation");
        if (!root_check) return root_check;
        auto lock_check = ensure_locked();
        if (!lock_check) return lock_check;

        // A local .wsh file installs from a bundle instead of the repo -- this
        // is how a `wish bundle` output gets deployed on another machine.
        bool looks_local = pkg_name.size() > 4 &&
                           pkg_name.compare(pkg_name.size() - 4, 4, ".wsh") == 0;
        if (looks_local || fs::exists(pkg_name)) {
            if (!fs::is_regular_file(pkg_name))
                return Result<void>::failure("Not a bundle file: " + pkg_name);
            if (dry_run) {
                std::cout << "Would install bundle: " << pkg_name << "\n";
                return Result<void>::success();
            }
            return install_bundle(pkg_name);
        }

        if (!PathValidator::is_safe_package_name(pkg_name)) {
            return Result<void>::failure("Invalid package name");
        }

        if (db_.is_installed(pkg_name)) {
            Logger::info("Package already installed: " + pkg_name);
            return Result<void>::success();
        }

        return install_impl(pkg_name, dry_run);
    }
    
    Result<void> remove(const std::string& pkg_name, bool force = false, bool dry_run = false) {
        auto root_check = Security::require_root("Removal");
        if (!root_check) return root_check;
        auto lock_check = ensure_locked();
        if (!lock_check) return lock_check;

        if (!PathValidator::is_safe_package_name(pkg_name)) {
            return Result<void>::failure("Invalid package name");
        }

        Logger::info("Removing: " + pkg_name);

        auto pkg_result = db_.get(pkg_name);
        if (!pkg_result) {
            return Result<void>::failure("Package not installed: " + pkg_name);
        }

        // Reverse-dependency protection: refuse to pull the rug out from
        // under something that still needs this package. --force overrides
        // (the caller is on their own for whatever breaks).
        if (!force) {
            auto dependents = db_.reverse_depends(pkg_name);
            if (!dependents.empty()) {
                std::string list;
                for (const auto& d : dependents) list += (list.empty() ? "" : ", ") + d;
                return Result<void>::failure(
                    pkg_name + " is still required by: " + list +
                    " (use --force to remove anyway)");
            }
        }

        auto pkg = pkg_result.value();

        if (dry_run) {
            std::cout << "Would remove " << pkg_name << " (" << human_size(pkg.installed_size)
                      << ", " << pkg.files.size() << " file(s))\n";
            return Result<void>::success();
        }

        // Remove files
        for (const auto& file : pkg.files) {
            if (fs::exists(file) && !fs::is_directory(file)) {
                try {
                    fs::remove(file);
                } catch (const fs::filesystem_error& e) {
                    Logger::warn("Cannot remove: " + file);
                }
            }
        }

        // Remove from cache
        std::string cache_dir = Config::get_cache_dir();
        std::string pkg_file = pkg.filename();
        try {
            fs::remove(cache_dir + "/" + pkg_file);
            fs::remove(cache_dir + "/" + pkg_file + ".sha256");
        } catch (...) {}

        db_.remove(pkg_name);
        history_.record("remove", pkg_name, {}, pkg.files);

        // Snapshot the post-removal state so it can be rolled back to / from.
        auto snap = gen_.snapshot("remove " + pkg_name, db_.get_all(),
                                  Config::get_root_dir(), Config::get_managed_config_paths());
        if (snap) Logger::ok("Created generation " + std::to_string(snap.value()));

        Logger::ok(pkg_name + " removed");
        return Result<void>::success();
    }

    // Reconcile the live package set to the one recorded in generation `id`:
    // remove anything installed since, (re)install anything missing or at a
    // different version (from cache, or re-downloaded if the .wsh is gone),
    // then move the `current` pointer. Atomic-ish and deterministic because a
    // generation is a full snapshot of the DB.
    Result<void> rollback(int id) {
        auto root_check = Security::require_root("Rollback");
        if (!root_check) return root_check;

        auto target = gen_.get(id);
        if (!target) return Result<void>::failure(target.error());

        Logger::info("Rolling back to generation " + std::to_string(id));
        auto want = target.value().packages;
        auto have = db_.get_all();

        // 1) Remove packages that exist now but not in the target generation.
        for (const auto& [name, pkg] : have) {
            if (want.count(name)) continue;
            Logger::info("Removing " + name);
            for (const auto& f : pkg.files) {
                std::error_code ec;
                if (fs::exists(f) && !fs::is_directory(f)) fs::remove(f, ec);
            }
            db_.remove(name);
        }

        // 2) Restore packages present in the target but missing/changed now.
        for (const auto& [name, pkg] : want) {
            auto cur = have.find(name);
            bool need = (cur == have.end()) ||
                        cur->second.version != pkg.version ||
                        cur->second.release != pkg.release;
            if (!need) continue;

            Logger::info("Restoring " + name + " " + pkg.version);
            std::string local = Config::get_cache_dir() + "/" + pkg.filename();
            if (!fs::exists(local)) {
                if (index_.get_all().empty()) update();
                auto ir = index_.get(name);
                if (!ir) { Logger::warn("Cannot restore " + name + ": not in cache or index"); continue; }
                auto dl = download_from_source(ir.value());
                if (!dl) { Logger::warn("Cannot restore " + name + ": " + dl.error()); continue; }
                local = dl.value();
            }
            auto ex = ArchiveExtractor::extract(local, Config::get_root_dir());
            if (!ex) { Logger::warn("Cannot extract " + name); continue; }
            db_.install(pkg, ex.value());
        }

        // 3) Restore config/filesystem state captured with this generation.
        gen_.restore_config(id, Config::get_root_dir(), Config::get_managed_config_paths());

        gen_.set_current(id);
        history_.record("rollback", "generation-" + std::to_string(id), {}, {});
        Logger::ok("Rolled back to generation " + std::to_string(id));
        return Result<void>::success();
    }

    Result<void> generation_list() {
        return gen_.list();
    }

    Result<void> generation_create() {
        auto root_check = Security::require_root("Generation create");
        if (!root_check) return root_check;
        auto snap = gen_.snapshot("manual", db_.get_all(),
                                  Config::get_root_dir(), Config::get_managed_config_paths());
        if (!snap) return Result<void>::failure(snap.error());
        Logger::ok("Created generation " + std::to_string(snap.value()));
        return Result<void>::success();
    }

    // ---- Bundling: repackage installed state back into a .wsh ----

    // Strip the install-root prefix off an absolute installed-file path to get
    // the archive-relative name (e.g. /wtest/root/usr/bin/x -> usr/bin/x).
    static std::string strip_root(const std::string& fp, const std::string& root) {
        std::string r = root;
        while (r.size() > 1 && r.back() == '/') r.pop_back();
        std::string s = fp;
        if (r != "/" && s.compare(0, r.size(), r) == 0) s = s.substr(r.size());
        while (!s.empty() && s.front() == '/') s.erase(s.begin());
        return s;
    }

    // Append (disk,arc) pairs for a set of files plus their ancestor dirs so the
    // payload extracts with correct directory structure.
    void add_files_with_dirs(const std::vector<std::string>& files, const std::string& root,
                             std::vector<std::pair<std::string, std::string>>& entries) {
        std::set<std::string> dirs;
        std::vector<std::pair<std::string, std::string>> file_entries;
        for (const auto& fp : files) {
            if (!fs::exists(fp)) continue;
            std::string arc = strip_root(fp, root);
            if (arc.empty()) continue;
            file_entries.push_back({fp, arc});
            // record ancestor dirs
            fs::path p = fs::path(arc).parent_path();
            while (!p.empty() && p != p.root_path()) { dirs.insert(p.string()); p = p.parent_path(); }
        }
        std::vector<std::string> sorted_dirs(dirs.begin(), dirs.end());
        std::sort(sorted_dirs.begin(), sorted_dirs.end(),
                  [](const std::string& a, const std::string& b){ return a.size() < b.size(); });
        std::string r = root;
        while (r.size() > 1 && r.back() == '/') r.pop_back();
        for (const auto& d : sorted_dirs) entries.push_back({(r == "/" ? "" : r) + "/" + d, d});
        for (auto& fe : file_entries) entries.push_back(fe);
    }

    std::string make_temp_dir(const std::string& tag) {
        std::string base = Config::get_cache_dir() + "/" + tag + "-" +
                           std::to_string(getpid()) + "-" + std::to_string(time(nullptr));
        std::error_code ec;
        fs::create_directories(base, ec);
        return base;
    }

    Result<void> bundle(const std::string& name) {
        if (!PathValidator::is_safe_package_name(name))
            return Result<void>::failure("Invalid package name");
        auto pr = db_.get(name);
        if (!pr) return Result<void>::failure("Package not installed: " + name);
        PackageInfo p = pr.value();

        Logger::info("Bundling " + name + " " + p.version + " (with config)");
        std::string root = Config::get_root_dir();

        // Build payload: package files + this package's wish overrides dir.
        std::vector<std::pair<std::string, std::string>> entries;
        add_files_with_dirs(p.files, root, entries);

        std::string ov_rel = "etc/wish/package-overrides/" + name;
        std::string r = root;
        while (r.size() > 1 && r.back() == '/') r.pop_back();
        std::string ov_disk = (r == "/" ? "" : r) + "/" + ov_rel;
        bool has_overrides = fs::exists(ov_disk);
        if (has_overrides) ArchivePacker::expand(ov_disk, ov_rel, entries);

        std::string tmp = make_temp_dir("bundle");
        std::string payload = tmp + "/payload.tar.gz";
        auto pr2 = ArchivePacker::create_gz(payload, entries, {});
        if (!pr2) { fs::remove_all(tmp); return pr2; }

        json meta;
        meta["type"] = "package";
        meta["wish_version"] = Config::VERSION;
        meta["has_overrides"] = has_overrides;
        meta["package"] = p.to_json();

        std::string arch = p.arch.empty() ? Config::get_arch() : p.arch;
        std::string out = name + "-" + p.version + "-custom-" + arch + ".wsh";
        auto ob = ArchivePacker::create_gz(out, {{payload, "payload.tar.gz"}},
                                           {{"metadata.json", meta.dump(2)}});
        fs::remove_all(tmp);
        if (!ob) return ob;

        Logger::ok("Bundle created: " + out + (has_overrides ? " (with overrides)" : ""));
        return Result<void>::success();
    }

    Result<void> bundle_system() {
        auto installed = db_.get_all();

        Logger::info("Bundling entire system (" + std::to_string(installed.size()) + " packages + config)");
        std::string root = Config::get_root_dir();

        std::vector<std::pair<std::string, std::string>> entries;
        for (const auto& [n, pkg] : installed)
            add_files_with_dirs(pkg.files, root, entries);

        // Managed config subtrees (e.g. /etc)
        std::string r = root;
        while (r.size() > 1 && r.back() == '/') r.pop_back();
        for (const auto& mp : Config::get_managed_config_paths()) {
            std::string disk = (r == "/" ? "" : r) + "/" + mp;
            if (fs::exists(disk)) ArchivePacker::expand(disk, mp, entries);
        }

        std::string tmp = make_temp_dir("sysbundle");
        std::string payload = tmp + "/payload.tar.gz";
        auto pr2 = ArchivePacker::create_gz(payload, entries, {});
        if (!pr2) { fs::remove_all(tmp); return pr2; }

        // Embed the wish state (package DB, federation, services) in metadata.
        json meta;
        meta["type"] = "system";
        meta["wish_version"] = Config::VERSION;
        meta["timestamp"] = Logger::timestamp();
        json inst = json::object();
        for (const auto& [n, pkg] : installed) inst[n] = pkg.to_json();
        meta["installed"] = inst;

        std::string fed = Config::get_lib_dir() + "/federation.json";
        if (fs::exists(fed)) {
            try { std::ifstream f(fed); json j; f >> j; meta["federation"] = j; } catch (...) {}
        }
        meta["services"] = json::array();
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(Config::get_services_dir(), ec)) {
            if (e.path().extension() != ".json") continue;
            try { std::ifstream f(e.path()); json j; f >> j; meta["services"].push_back(j); } catch (...) {}
        }

        std::time_t t = time(nullptr);
        char datebuf[32];
        std::strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", std::localtime(&t));
        std::string out = std::string("system-") + datebuf + ".wsh";
        auto ob = ArchivePacker::create_gz(out, {{payload, "payload.tar.gz"}},
                                           {{"metadata.json", meta.dump(2)}});
        fs::remove_all(tmp);
        if (!ob) return ob;

        Logger::ok("System bundle created: " + out);
        return Result<void>::success();
    }

    Result<void> install_bundle(const std::string& path) {
        Logger::info("Installing from bundle: " + path);
        std::string tmp = make_temp_dir("unbundle");
        auto ex = ArchiveExtractor::extract(path, tmp);
        if (!ex) { fs::remove_all(tmp); return Result<void>::failure("Cannot open bundle: " + ex.error()); }

        std::string meta_path = tmp + "/metadata.json";
        if (!fs::exists(meta_path)) { fs::remove_all(tmp); return Result<void>::failure("Not a wish bundle (no metadata.json)"); }

        json meta;
        try { std::ifstream f(meta_path); f >> meta; }
        catch (...) { fs::remove_all(tmp); return Result<void>::failure("Corrupt bundle metadata"); }

        std::string type = meta.value("type", "package");
        std::string payload = tmp + "/payload.tar.gz";
        if (!fs::exists(payload)) { fs::remove_all(tmp); return Result<void>::failure("Bundle has no payload"); }

        std::string root = Config::get_root_dir();

        if (type == "system") {
            auto r = restore_system(payload, meta, root);
            fs::remove_all(tmp);
            return r;
        }

        // --- Package bundle ---
        Transaction tx;
        PackageInfo p = PackageInfo::from_json(meta["package"]);
        tx.begin("install-bundle", p.name);
        tx.stage_ok("Read bundle metadata");

        auto files = ArchiveExtractor::extract(payload, root);
        if (!files) {
            tx.stage_fail("Extract payload");
            tx.rollback(files.error());
            fs::remove_all(tmp);
            return Result<void>::failure("Extraction failed: " + files.error());
        }
        tx.track(files.value());
        tx.stage_ok("Extract files + config");

        db_.install(p, files.value());
        history_.record("install-bundle", p.name, {}, files.value());

        // Run the bundle's post-install hook if it shipped one. When installing
        // into a non-"/" root, chroot into it first so the hook's absolute paths
        // resolve inside the target (and it cannot write outside it).
        std::string r = root;
        while (r.size() > 1 && r.back() == '/') r.pop_back();
        std::string hook_rel = "/etc/wish/package-overrides/" + p.name + "/scripts/post-install";
        std::string hook_disk = (r == "/" ? "" : r) + hook_rel;
        if (fs::exists(hook_disk)) {
            Logger::info("Running post-install hook");
            pid_t pid = fork();
            if (pid == 0) {
                if (r != "/") {
                    if (chroot(r.c_str()) != 0 || chdir("/") != 0) {
                        std::cerr << "[error] post-install chroot failed: " << strerror(errno) << "\n";
                        _exit(126);
                    }
                }
                execl("/bin/sh", "sh", hook_rel.c_str(), (char*)nullptr);
                _exit(127);
            } else if (pid > 0) {
                int st; waitpid(pid, &st, 0);
            }
        }
        tx.stage_ok("Apply config/scripts");

        tx.commit();
        auto snap = gen_.snapshot("install-bundle " + p.name, db_.get_all(),
                                  Config::get_root_dir(), Config::get_managed_config_paths());
        tx.stage_ok("Commit");
        if (snap) Logger::ok("Created generation " + std::to_string(snap.value()));

        fs::remove_all(tmp);
        Logger::ok(p.name + " installed from bundle");
        return Result<void>::success();
    }

    Result<void> restore_system(const std::string& payload, const json& meta, const std::string& root) {
        Logger::info("Restoring system from bundle...");

        // 1) Filesystem: extract all files + config.
        auto files = ArchiveExtractor::extract(payload, root);
        if (!files) return Result<void>::failure("Restore extraction failed: " + files.error());
        Logger::ok("Restored " + std::to_string(files.value().size()) + " files");

        // 2) Package database.
        if (meta.contains("installed")) {
            json db;
            db["installed"] = meta["installed"];
            write_file_atomic(Config::get_lib_dir() + "/installed.json", db.dump(2));
            Logger::ok("Restored package database (" +
                       std::to_string(meta["installed"].size()) + " packages)");
        }

        // 3) Federation config.
        if (meta.contains("federation") && !meta["federation"].is_null()) {
            write_file_atomic(Config::get_lib_dir() + "/federation.json", meta["federation"].dump(2));
            Logger::ok("Restored federation config");
        }

        // 4) Service definitions.
        if (meta.contains("services")) {
            std::error_code ec;
            fs::create_directories(Config::get_services_dir(), ec);
            int n = 0;
            for (const auto& svc : meta["services"]) {
                std::string sname = svc.value("name", "");
                if (sname.empty()) continue;
                write_file_atomic(Config::get_services_dir() + "/" + sname + ".json", svc.dump(2));
                n++;
            }
            if (n) Logger::ok("Restored " + std::to_string(n) + " service definitions");
        }

        auto snap = gen_.snapshot("restore-system", db_.get_all(),
                                  Config::get_root_dir(), Config::get_managed_config_paths());
        if (snap) Logger::ok("Created generation " + std::to_string(snap.value()));

        Logger::ok("System restored");
        return Result<void>::success();
    }

    Result<void> restore(const std::string& path) {
        auto root_check = Security::require_root("Restore");
        if (!root_check) return root_check;
        if (!fs::is_regular_file(path)) return Result<void>::failure("Not a bundle file: " + path);
        return install_bundle(path);
    }

    Result<void> upgrade() {
        auto root_check = Security::require_root("Upgrade");
        if (!root_check) return root_check;
        auto lock_check = ensure_locked();
        if (!lock_check) return lock_check;

        Logger::info("Upgrading all packages...");

        auto update_result = update();
        if (!update_result) return update_result;

        auto installed = db_.get_all();
        int upgraded = 0;

        for (const auto& [name, pkg] : installed) {
            if (pkg.pinned) {
                Logger::info(name + " is pinned, skipping");
                continue;
            }
            auto remote = index_.get(name);
            if (!remote) continue;

            auto remote_pkg = remote.value();
            if (remote_pkg.version != pkg.version || remote_pkg.release > pkg.release) {
                Logger::info("Upgrading " + name + " " + pkg.version + " -> " + remote_pkg.version);

                auto install_result = install_impl(name);
                if (install_result) {
                    upgraded++;
                }
            }
        }

        Logger::ok("Upgraded " + std::to_string(upgraded) + " packages");
        return Result<void>::success();
    }

    Result<void> search(const std::string& query) {
        if (index_.get_all().empty()) {
            auto update_result = update();
            if (!update_result) return update_result;
        }
        return index_.search(query);
    }

    Result<void> list() {
        return db_.list();
    }

    Result<void> pull(const std::string& pkg_name) {
        if (!PathValidator::is_safe_package_name(pkg_name)) {
            return Result<void>::failure("Invalid package name");
        }

        Logger::info("Pulling: " + pkg_name);

        if (index_.get_all().empty()) {
            auto update_result = update();
            if (!update_result) return update_result;
        }

        auto pkg_result = index_.get(pkg_name);
        if (!pkg_result) {
            return Result<void>::failure("Package not found: " + pkg_name);
        }

        auto dl_result = download_from_source(pkg_result.value());
        if (!dl_result) {
            return Result<void>::failure("Download failed: " + dl_result.error());
        }

        Logger::ok(pkg_name + " downloaded to cache");
        return Result<void>::success();
    }

    Result<void> show_graph(const std::string& pkg_name) {
        if (index_.get_all().empty()) {
            auto update_result = update();
            if (!update_result) return update_result;
        }

        return resolver_.show_graph(pkg_name);
    }

    Result<void> pin(const std::string& pkg_name) {
        auto root_check = Security::require_root("Pin");
        if (!root_check) return root_check;
        auto lock_check = ensure_locked();
        if (!lock_check) return lock_check;
        auto r = db_.set_pinned(pkg_name, true);
        if (r) Logger::ok(pkg_name + " pinned (upgrades will skip it)");
        return r;
    }

    Result<void> unpin(const std::string& pkg_name) {
        auto root_check = Security::require_root("Unpin");
        if (!root_check) return root_check;
        auto lock_check = ensure_locked();
        if (!lock_check) return lock_check;
        auto r = db_.set_pinned(pkg_name, false);
        if (r) Logger::ok(pkg_name + " unpinned");
        return r;
    }

    Result<void> owns(const std::string& path) {
        auto r = db_.owning_file(path);
        if (!r) return Result<void>::failure(r.error());
        std::cout << path << " is owned by " << r.value() << "\n";
        return Result<void>::success();
    }

    Result<void> why(const std::string& name) {
        auto r = db_.why(name);
        if (!r) return Result<void>::failure(r.error());
        auto chain = r.value();
        if (chain.empty()) {
            std::cout << name << " is explicitly installed (nothing pulled it in).\n";
            return Result<void>::success();
        }
        std::cout << "Why is " << name << " installed:\n  ";
        for (size_t i = 0; i < chain.size(); i++) {
            if (i) std::cout << " -> ";
            std::cout << chain[i];
        }
        std::cout << "\n";
        return Result<void>::success();
    }

    // Diagnoses exactly what would block `wish install <name>` from
    // succeeding, instead of just returning ok/fail like resolve() does.
    Result<void> why_not(const std::string& name) {
        if (index_.get_all().empty()) {
            auto update_result = update();
            if (!update_result) return update_result;
        }

        auto direct = index_.get(name);
        if (!direct) {
            auto providers = index_.get_providers(name);
            if (providers.empty()) {
                std::cout << name << " does not exist in the catalog under this name, "
                          << "and nothing provides it as a virtual package.\n";
            } else {
                std::cout << name << " is not a real package, but is provided by: ";
                for (size_t i = 0; i < providers.size(); i++) {
                    if (i) std::cout << ", ";
                    std::cout << providers[i];
                }
                std::cout << "\n(install one of those instead)\n";
            }
            return Result<void>::success();
        }

        auto result = resolver_.resolve(name);
        if (!result) {
            std::cout << name << " cannot be installed: " << result.error() << "\n";
            return Result<void>::success();
        }

        std::cout << name << " has no known obstruction -- resolves to "
                  << result.value().size() << " package(s) and should install cleanly.\n";
        return Result<void>::success();
    }

    Result<void> list_orphans() {
        auto orphans = db_.find_orphans();
        if (orphans.empty()) {
            std::cout << "No orphaned packages.\n";
            return Result<void>::success();
        }
        std::cout << "Orphaned packages (auto-installed, nothing depends on them):\n";
        for (const auto& name : orphans) std::cout << "  " << name << "\n";
        return Result<void>::success();
    }

    Result<void> autoremove() {
        auto root_check = Security::require_root("Autoremove");
        if (!root_check) return root_check;
        auto lock_check = ensure_locked();
        if (!lock_check) return lock_check;

        auto orphans = db_.find_orphans();
        if (orphans.empty()) {
            Logger::ok("Nothing to autoremove");
            return Result<void>::success();
        }
        Logger::info("Removing " + std::to_string(orphans.size()) + " orphaned package(s):");
        for (const auto& name : orphans) std::cout << "  " << name << "\n";

        int removed = 0;
        for (const auto& name : orphans) {
            // Safe to force here -- find_orphans() already computed the
            // whole chain together, so nothing still-needed is in this list.
            auto r = remove(name, /*force=*/true);
            if (r) removed++;
            else Logger::warn("Could not remove " + name + ": " + r.error());
        }
        Logger::ok("Autoremoved " + std::to_string(removed) + "/" + std::to_string(orphans.size()) + " package(s)");
        return Result<void>::success();
    }

    PeerServer& peer_server() { return peer_; }
};

// ============================================================
// CLI Parser
// ============================================================

struct Command {
    std::string name;
    std::vector<std::string> args;
    std::map<std::string, std::string> flags;
};

class CLIParser {
public:
    static Command parse(int argc, char** argv) {
        Command cmd;
        if (argc < 2) return cmd;

        std::string first = argv[1];

        // Handle flags like -I, --install
        if (first == "-I" || first == "--install") {
            cmd.name = "install";
            if (argc > 2) cmd.args.push_back(argv[2]);
        }
        else if (first == "--pull") {
            cmd.name = "pull";
            if (argc > 2) cmd.args.push_back(argv[2]);
        }
        else if (first == "--remove") {
            cmd.name = "remove";
            if (argc > 2) cmd.args.push_back(argv[2]);
        }
        else if (first == "--update") {
            cmd.name = "update";
        }
        else if (first == "--upgrade") {
            cmd.name = "upgrade";
        }
        else {
            cmd.name = first;
            for (int i = 2; i < argc; i++) {
                std::string arg = argv[i];
                if (arg.find("--") == 0) {
                    size_t eq = arg.find('=');
                    if (eq != std::string::npos) {
                        cmd.flags[arg.substr(2, eq - 2)] = arg.substr(eq + 1);
                    } else {
                        cmd.flags[arg.substr(2)] = "true";
                    }
                } else {
                    cmd.args.push_back(arg);
                }
            }
        }

        return cmd;
    }
};

// ============================================================
// Main Application
// ============================================================

class WishApplication {
    PackageManager pkg_mgr_;
    LayerManager layer_mgr_;
    HistoryManager history_;
    InitManager init_mgr_;
    ServiceManager svc_mgr_;
    Installer installer_;

public:
    int run(const Command& cmd) {
        if (cmd.name.empty()) {
            show_help();
            return 0;
        }

        if (cmd.name == "init") {
            auto result = init_mgr_.initialize_system();
            return result ? 0 : 1;
        }

        else if (cmd.name == "build-tool" || cmd.name == "--build-tool") {
            auto result = installer_.build_tool();
            if (!result) Logger::error(result.error());
            return result ? 0 : 1;
        }

        else if (cmd.name == "complete-install" || cmd.name == "--complete-install") {
            auto result = installer_.complete_install();
            if (!result) Logger::error(result.error());
            return result ? 0 : 1;
        }

        else if (cmd.name == "install" || cmd.name == "-I") {
            if (cmd.args.empty()) {
                Logger::error("Package name required");
                return 1;
            }
            bool dry_run = cmd.flags.count("dry-run") > 0;
            for (const auto& pkg : cmd.args) {
                auto result = pkg_mgr_.install(pkg, dry_run);
                if (!result) {
                    Logger::error(result.error());
                    return 1;
                }
            }
            return 0;
        }

        else if (cmd.name == "remove" || cmd.name == "--remove") {
            if (cmd.args.empty()) {
                Logger::error("Package name required");
                return 1;
            }
            bool force = cmd.flags.count("force") > 0;
            bool dry_run = cmd.flags.count("dry-run") > 0;
            for (const auto& pkg : cmd.args) {
                auto result = pkg_mgr_.remove(pkg, force, dry_run);
                if (!result) {
                    Logger::error(result.error());
                    return 1;
                }
            }
            return 0;
        }

        else if (cmd.name == "pin") {
            if (cmd.args.empty()) { Logger::error("Package name required"); return 1; }
            auto result = pkg_mgr_.pin(cmd.args[0]);
            if (!result) Logger::error(result.error());
            return result ? 0 : 1;
        }

        else if (cmd.name == "unpin") {
            if (cmd.args.empty()) { Logger::error("Package name required"); return 1; }
            auto result = pkg_mgr_.unpin(cmd.args[0]);
            if (!result) Logger::error(result.error());
            return result ? 0 : 1;
        }

        else if (cmd.name == "owns") {
            if (cmd.args.empty()) { Logger::error("File path required"); return 1; }
            auto result = pkg_mgr_.owns(cmd.args[0]);
            if (!result) Logger::error(result.error());
            return result ? 0 : 1;
        }

        else if (cmd.name == "orphans") {
            auto result = pkg_mgr_.list_orphans();
            return result ? 0 : 1;
        }

        else if (cmd.name == "autoremove") {
            auto result = pkg_mgr_.autoremove();
            if (!result) Logger::error(result.error());
            return result ? 0 : 1;
        }

        else if (cmd.name == "why") {
            if (cmd.args.empty()) { Logger::error("Package name required"); return 1; }
            auto result = pkg_mgr_.why(cmd.args[0]);
            if (!result) Logger::error(result.error());
            return result ? 0 : 1;
        }

        else if (cmd.name == "why-not") {
            if (cmd.args.empty()) { Logger::error("Package name required"); return 1; }
            auto result = pkg_mgr_.why_not(cmd.args[0]);
            if (!result) Logger::error(result.error());
            return result ? 0 : 1;
        }

        else if (cmd.name == "update" || cmd.name == "--update") {
            auto result = pkg_mgr_.update();
            if (!result) Logger::error(result.error());
            return result ? 0 : 1;
        }

        else if (cmd.name == "upgrade" || cmd.name == "--upgrade") {
            auto result = pkg_mgr_.upgrade();
            if (!result) Logger::error(result.error());
            return result ? 0 : 1;
        }

        else if (cmd.name == "pull" || cmd.name == "--pull") {
            if (cmd.args.empty()) {
                Logger::error("Package name required");
                return 1;
            }
            auto result = pkg_mgr_.pull(cmd.args[0]);
            if (!result) Logger::error(result.error());
            return result ? 0 : 1;
        }

        else if (cmd.name == "search") {
            if (cmd.args.empty()) {
                Logger::error("Search query required");
                return 1;
            }
            auto result = pkg_mgr_.search(cmd.args[0]);
            if (!result) Logger::error(result.error());
            return result ? 0 : 1;
        }

        else if (cmd.name == "list") {
            auto result = pkg_mgr_.list();
            return result ? 0 : 1;
        }

        else if (cmd.name == "graph") {
            if (cmd.flags.count("history")) {
                auto result = history_.show_history();
                return result ? 0 : 1;
            }
            if (cmd.args.empty()) {
                Logger::error("Package name required");
                return 1;
            }
            auto result = pkg_mgr_.show_graph(cmd.args[0]);
            return result ? 0 : 1;
        }

        else if (cmd.name == "serve") {
            auto result = pkg_mgr_.peer_server().start();
            if (!result) {
                Logger::error(result.error());
                return 1;
            }
            std::cout << "Press Enter to stop server...\n";
            std::cin.get();
            pkg_mgr_.peer_server().stop();
            return 0;
        }

        else if (cmd.name == "peer") {
            if (!cmd.args.empty() && cmd.args[0] == "scan") {
                auto result = pkg_mgr_.peer_server().scan_peers();
                if (result) {
                    std::cout << "Found peers:\n";
                    for (const auto& peer : result.value()) {
                        std::cout << "  " << peer << "\n";
                    }
                }
                return result ? 0 : 1;
            }
            Logger::error("Usage: wish peer scan");
            return 1;
        }

        else if (cmd.name == "layer") {
            if (cmd.args.empty()) {
                auto result = layer_mgr_.list();
                return result ? 0 : 1;
            }

            std::string action = cmd.args[0];
            if (action == "list") {
                auto result = layer_mgr_.list();
                return result ? 0 : 1;
            }
            else if (action == "add") {
                if (cmd.args.size() < 2) {
                    Logger::error("Layer name required");
                    return 1;
                }
                auto result = layer_mgr_.add(cmd.args[1]);
                if (!result) Logger::error(result.error());
                return result ? 0 : 1;
            }
            else if (action == "remove") {
                if (cmd.args.size() < 2) {
                    Logger::error("Layer name required");
                    return 1;
                }
                auto result = layer_mgr_.remove(cmd.args[1]);
                if (!result) Logger::error(result.error());
                return result ? 0 : 1;
            }
            else if (action == "info") {
                if (cmd.args.size() < 2) { Logger::error("Layer name required"); return 1; }
                auto result = layer_mgr_.info(cmd.args[1]);
                if (!result) Logger::error(result.error());
                return result ? 0 : 1;
            }
            else if (action == "share") {
                if (cmd.args.size() < 3) {
                    Logger::error("Usage: wish layer share <name> <source> [target] [--ro]");
                    return 1;
                }
                std::string target = cmd.args.size() >= 4 ? cmd.args[3] : "";
                bool ro = cmd.flags.count("ro") > 0;
                auto result = layer_mgr_.share(cmd.args[1], cmd.args[2], target, ro);
                if (!result) Logger::error(result.error());
                return result ? 0 : 1;
            }
            else if (action == "unshare") {
                if (cmd.args.size() < 3) {
                    Logger::error("Usage: wish layer unshare <name> <target>");
                    return 1;
                }
                auto result = layer_mgr_.unshare_target(cmd.args[1], cmd.args[2]);
                if (!result) Logger::error(result.error());
                return result ? 0 : 1;
            }
            else if (action == "overlay") {
                if (cmd.args.size() < 2) {
                    Logger::error("Usage: wish layer overlay <name> [add <lowerdir> | off]");
                    return 1;
                }
                std::string sub = cmd.args.size() >= 3 ? cmd.args[2] : "";
                std::string lower = cmd.args.size() >= 4 ? cmd.args[3] : "";
                auto result = layer_mgr_.overlay(cmd.args[1], sub, lower);
                if (!result) Logger::error(result.error());
                return result ? 0 : 1;
            }
            else if (action == "clone") {
                if (cmd.args.size() < 3) {
                    Logger::error("Usage: wish layer clone <src> <dst>");
                    return 1;
                }
                auto result = layer_mgr_.clone(cmd.args[1], cmd.args[2]);
                if (!result) Logger::error(result.error());
                return result ? 0 : 1;
            }
            else if (action == "snapshot") {
                if (cmd.args.size() < 2) {
                    Logger::error("Usage: wish layer snapshot <name> [snapshot-name]");
                    return 1;
                }
                std::string snap = cmd.args.size() >= 3 ? cmd.args[2] : "";
                auto result = layer_mgr_.snapshot(cmd.args[1], snap);
                if (!result) Logger::error(result.error());
                return result ? 0 : 1;
            }
            else if (action == "gui") {
                if (cmd.args.size() < 3 || (cmd.args[2] != "on" && cmd.args[2] != "off")) {
                    Logger::error("Usage: wish layer gui <name> <on|off>");
                    return 1;
                }
                auto result = layer_mgr_.gui_toggle(cmd.args[1], cmd.args[2] == "on");
                if (!result) Logger::error(result.error());
                return result ? 0 : 1;
            }
            else if (action == "expose") {
                if (cmd.args.size() < 3) {
                    Logger::error("Usage: wish layer expose <name> <binary> [alias]");
                    return 1;
                }
                std::string alias = cmd.args.size() >= 4 ? cmd.args[3] : "";
                auto result = layer_mgr_.expose(cmd.args[1], cmd.args[2], alias);
                if (!result) Logger::error(result.error());
                return result ? 0 : 1;
            }
            else if (action == "unexpose") {
                if (cmd.args.size() < 2) {
                    Logger::error("Usage: wish layer unexpose <alias>");
                    return 1;
                }
                auto result = layer_mgr_.unexpose(cmd.args[1]);
                if (!result) Logger::error(result.error());
                return result ? 0 : 1;
            }
            else if (action == "priority") {
                if (cmd.args.size() < 2) {
                    auto result = layer_mgr_.priority_show();
                    return result ? 0 : 1;
                }
                if (cmd.args[1] == "set") {
                    std::vector<std::string> order(cmd.args.begin() + 2, cmd.args.end());
                    if (order.empty()) { Logger::error("Usage: wish layer priority set <l1> <l2> ..."); return 1; }
                    auto result = layer_mgr_.priority_set(order);
                    if (!result) Logger::error(result.error());
                    return result ? 0 : 1;
                }
                Logger::error("Usage: wish layer priority [set <l1> <l2> ...]");
                return 1;
            }
            else {
                Logger::error("Unknown layer command: " + action);
                std::cerr << "Usage: wish layer [list|add|remove|info|share|unshare|overlay|clone|snapshot|gui|expose|unexpose|priority]\n";
                return 1;
            }
        }

        else if (cmd.name == "run") {
            if (cmd.args.size() < 2) {
                Logger::error("Usage: wish run <layer> <command> [args...]");
                return 1;
            }
            std::string layer = cmd.args[0];
            std::vector<std::string> run_args(cmd.args.begin() + 1, cmd.args.end());
            auto result = layer_mgr_.run(layer, run_args);
            if (!result) Logger::error(result.error());
            return result ? 0 : 1;
        }

        else if (cmd.name == "generation" || cmd.name == "gen") {
            if (cmd.args.empty() || cmd.args[0] == "list") {
                auto result = pkg_mgr_.generation_list();
                return result ? 0 : 1;
            }
            else if (cmd.args[0] == "create") {
                auto result = pkg_mgr_.generation_create();
                if (!result) Logger::error(result.error());
                return result ? 0 : 1;
            }
            else {
                Logger::error("Unknown generation command: " + cmd.args[0]);
                std::cerr << "Usage: wish generation [list|create]\n";
                return 1;
            }
        }

        else if (cmd.name == "rollback") {
            if (cmd.args.empty()) {
                Logger::error("Generation id required (see 'wish generation list')");
                return 1;
            }
            int id = 0;
            try {
                id = std::stoi(cmd.args[0]);
            } catch (...) {
                Logger::error("Invalid generation id: " + cmd.args[0]);
                return 1;
            }
            auto result = pkg_mgr_.rollback(id);
            if (!result) Logger::error(result.error());
            return result ? 0 : 1;
        }

        else if (cmd.name == "service" || cmd.name == "svc") {
            if (cmd.args.empty()) {
                auto result = svc_mgr_.list();
                return result ? 0 : 1;
            }
            std::string action = cmd.args[0];

            if (action == "list") {
                auto result = svc_mgr_.list();
                return result ? 0 : 1;
            }
            else if (action == "define" || action == "add") {
                if (cmd.args.size() < 3) {
                    Logger::error("Usage: wish service define <name> <command> [--restart=<policy>] [--enable]");
                    return 1;
                }
                std::string policy = cmd.flags.count("restart") ? cmd.flags.at("restart") : "no";
                bool enable = cmd.flags.count("enable") > 0;
                auto result = svc_mgr_.define(cmd.args[1], cmd.args[2], policy, enable);
                if (!result) Logger::error(result.error());
                return result ? 0 : 1;
            }
            else if (action == "start" || action == "stop" || action == "status" ||
                     action == "enable" || action == "disable") {
                if (cmd.args.size() < 2) {
                    Logger::error("Service name required");
                    return 1;
                }
                const std::string& name = cmd.args[1];
                Result<void> result = Result<void>::success();
                if (action == "start")        result = svc_mgr_.start(name);
                else if (action == "stop")    result = svc_mgr_.stop(name);
                else if (action == "status")  result = svc_mgr_.status(name);
                else if (action == "enable")  result = svc_mgr_.set_enabled(name, true);
                else if (action == "disable") result = svc_mgr_.set_enabled(name, false);
                if (!result) Logger::error(result.error());
                return result ? 0 : 1;
            }
            else {
                Logger::error("Unknown service command: " + action);
                std::cerr << "Usage: wish service [define|start|stop|status|enable|disable|list]\n";
                return 1;
            }
        }

        else if (cmd.name == "bundle" || cmd.name == "export" || cmd.name == "pack") {
            // --system (as a flag or as the argument) snapshots the whole system.
            bool system = cmd.flags.count("system") > 0 ||
                          (!cmd.args.empty() && cmd.args[0] == "--system");
            if (system) {
                auto result = pkg_mgr_.bundle_system();
                if (!result) Logger::error(result.error());
                return result ? 0 : 1;
            }
            if (cmd.args.empty()) {
                Logger::error("Usage: wish bundle <package> | wish bundle --system");
                return 1;
            }
            auto result = pkg_mgr_.bundle(cmd.args[0]);
            if (!result) Logger::error(result.error());
            return result ? 0 : 1;
        }

        else if (cmd.name == "restore") {
            if (cmd.args.empty()) {
                Logger::error("Usage: wish restore <system-bundle.wsh>");
                return 1;
            }
            auto result = pkg_mgr_.restore(cmd.args[0]);
            if (!result) Logger::error(result.error());
            return result ? 0 : 1;
        }

        else if (cmd.name == "federate") {
            auto result = layer_mgr_.federate();
            if (!result) Logger::error(result.error());
            return result ? 0 : 1;
        }

        else if (cmd.name == "defederate") {
            auto result = layer_mgr_.defederate();
            if (!result) Logger::error(result.error());
            return result ? 0 : 1;
        }

        else if (cmd.name == "info") {
            show_info();
            return 0;
        }

        else if (cmd.name == "help" || cmd.name == "--help" || cmd.name == "-h") {
            show_help();
            return 0;
        }

        else {
            Logger::error("Unknown command: " + cmd.name);
            std::cerr << "Use 'wish help' for usage information\n";
            return 1;
        }
    }

    void show_info() {
        std::cout << "WishOS System Manager\n"
                  << "Version: " << Config::VERSION << "\n"
                  << "Repository: " << Config::get_repo_url() << "\n"
                  << "Architecture: " << Config::get_arch() << "\n"
                  << "Cache: " << Config::get_cache_dir() << "\n"
                  << "Lib: " << Config::get_lib_dir() << "\n"
                  << "Peer Port: " << Config::PEER_PORT << "\n";
    }

    void show_help() {
        std::cout << R"(WishOS System Manager v2.0.0

Usage:
  wish <command> [options]

INIT SYSTEM:
  init                      Initialize system (mounts, dirs, databases)

INSTALLER:
  --build-tool               Minimal disk installer: partition, format,
                              copy system, set root password, install GRUB.
                              x86_64 only -- ARM ships as a prebuilt .img.
  --complete-install          Post-install account setup: create a user,
                              set password, optionally grant sudo.

PACKAGE MANAGEMENT:
  install <pkg> [--dry-run] Install package with dependencies
  -I, --install <pkg>       Alias for install
  remove <pkg> [--force] [--dry-run]
                             Remove installed package (refuses if another
                             installed package still depends on it, unless
                             --force)
  --remove <pkg>            Alias for remove
  update                    Update package index from repository
  --update                  Alias for update
  upgrade                   Upgrade all installed packages (skips pinned ones)
  --upgrade                 Alias for upgrade
  pull <pkg>                Download package to cache only
  --pull <pkg>              Alias for pull
  search <query>            Search packages in index
  list                      List installed packages (size, [auto], [pinned])
  pin <pkg>                 Hold a package at its current version
  unpin <pkg>               Release a pin
  owns <path>               Which installed package owns this file
  orphans                   List auto-installed packages nothing depends on
  autoremove                Remove every orphaned package
  why <pkg>                 Show the chain that pulled an auto-installed
                             package onto the system
  why-not <pkg>             Diagnose exactly what blocks installing <pkg>
                             (missing, version constraint, conflict)

DEPENDENCY MANAGEMENT:
  graph <pkg>               Show dependency tree for package
  graph --history           Show transaction history

BUNDLING (system -> package, the reverse direction):
  bundle <pkg>              Repackage an installed pkg + its config/overrides
                            into <pkg>-<ver>-custom-<arch>.wsh
  bundle --system          Snapshot the whole system (packages + config +
                            federation + services) into system-<date>.wsh
  export <pkg>              Alias for bundle
  install <file.wsh>        Install from a local bundle file
  restore <system.wsh>      Rehydrate a full system from a system bundle

GENERATIONS & ROLLBACK:
  generation list           List system generations (* = current)
  generation create         Snapshot current state as a new generation
  rollback <id>             Atomically revert system to generation <id>

SERVICE MANAGEMENT:
  service define <name> <cmd> [--restart=<policy>] [--enable]
                            Define a service (policy: no|always|on-failure)
  service start <name>      Start service daemon (tracks PID, redirects logs)
  service stop <name>       Stop service (SIGTERM, then SIGKILL)
  service status <name>     Show service status
  service enable <name>     Enable autostart at 'wish init'
  service disable <name>    Disable autostart
  service list              List all services

PEER NETWORKING (LAN):
  serve                     Start peer package server (port 44449)
  peer scan                 Scan local network for Wish peers

LAYER MANAGEMENT (Bedrock-style):
  layer list                List all layers
  layer add <name>          Create new layer (shares /home by default)
  layer remove <name>       Remove layer
  layer info <name>         Show layer shares + overlay config
  layer share <name> <src> [target] [--ro]
                            Bind-mount a host/cross-layer path into the layer
  layer unshare <name> <target>
                            Remove a share
  layer overlay <name> add <lowerdir>
                            Merge lowerdir under the layer (OverlayFS, COW)
  layer overlay <name> off  Disable overlay
  layer clone <src> <dst>   Copy a layer to a new one
  layer snapshot <name> [snap-name]
                            Snapshot a layer (timestamped by default)
  layer gui <name> <on|off> Share X11/Wayland/PipeWire/D-Bus sockets
  layer expose <name> <binary> [alias]
                            Put a wrapper on host PATH -> runs binary in layer
  layer unexpose <alias>    Remove an exposed wrapper
  run <layer> <cmd> [args]  Execute command in layer context

FEDERATION (Bedrock-style cross-layer sharing):
  layer priority            Show layer priority order
  layer priority set <l1> <l2> ...
                            Set priority (first wins name collisions)
  federate                  Expose all layers' commands + libraries globally,
                            resolving conflicts by priority
  defederate                Remove all federated commands + libraries

SYSTEM:
  info                      Show system information
  help, --help, -h          Show this help message

ENVIRONMENT VARIABLES:
  WISH_REPO_URL             Override repository URL
  WISH_CACHE_DIR            Override cache directory
  WISH_LIB_DIR              Override lib directory
  WISH_ROOT                 Override install root (default /)
  WISH_SERVICES_DIR         Override service definitions dir
  WISH_RUN_DIR              Override runtime PID dir

EXAMPLES:
  wish init                 # First-time system setup
  wish install curl         # Install curl with deps (transactional)
  wish -I mesa              # Short form install
  wish graph curl           # Show curl dependency tree
  wish generation list      # List system generations
  wish rollback 3           # Revert to generation 3
  wish service define web "httpd -f" --enable
  wish service start web    # Start the 'web' service
  wish serve                # Share packages on LAN
  wish layer add arch       # Create 'arch' layer
  wish run arch pacman -Q   # Run pacman in arch layer
)";
    }
};

// ============================================================
// Signal Handling
// ============================================================

static std::atomic<bool> g_running{true};

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_running = false;
    }
}

// ============================================================
// Main Entry Point
// ============================================================

int main(int argc, char** argv) {
    // Initialize CURL globally
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Set up signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Parse command line
    Command cmd = CLIParser::parse(argc, argv);

    // Run application
    WishApplication app;
    int ret = app.run(cmd);

    // Cleanup
    curl_global_cleanup();

    return ret;
}
