/* Portable generate → rebuild → relaunch host for recomp-ui setup wizards. */

#include "gbarecomp_codegen_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dirent.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <spawn.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

static const GbarecompCodegenHostConfig* g_cfg;
static char g_project_root[1024];
static char g_cli_path[1100];
static char g_python[512];
static char g_cmake[512];
static char g_build_dir[1100];
static char g_toolchain_bin[1100];
static char g_exe_path[1100];
static char g_helper_path[1100];
static char g_config[512];
static char g_out_dir[256];
static char g_bios[512];
static int g_has_bios;
static char g_cmake_target[256];
static char g_display[128];
static int g_ready;
static int g_relaunch_is_helper;

static const char* cfg_or(const char* v, const char* d) {
    return (v && v[0]) ? v : d;
}

static int path_is_file(const char* path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static int path_is_dir(const char* path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int join_path(char* out, size_t cap, const char* a, const char* b) {
    size_t na = strlen(a);
    int need_slash = na > 0 && a[na - 1] != '/' && a[na - 1] != '\\';
    int n = snprintf(out, cap, "%s%s%s", a, need_slash ? "/" : "", b);
    return n > 0 && (size_t)n < cap;
}

static int dirname_copy(char* out, size_t cap, const char* path) {
    size_t n = strlen(path);
    while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\'))
        --n;
    while (n > 0 && path[n - 1] != '/' && path[n - 1] != '\\')
        --n;
    while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\'))
        --n;
    if (n == 0) {
        if (cap < 2) return 0;
        out[0] = '.';
        out[1] = '\0';
        return 1;
    }
    if (n >= cap) return 0;
    memcpy(out, path, n);
    out[n] = '\0';
    return 1;
}

static int looks_like_project_root(const char* root) {
    char cli[1100], cfg[1100];
    if (!join_path(cli, sizeof(cli), root,
                   cfg_or(g_cfg->gbarecomp_cli_relpath,
                          "gbarecomp/gbarecomp_cli.py")))
        return 0;
    if (!join_path(cfg, sizeof(cfg), root,
                   cfg_or(g_cfg->seed_config_relpath, "CMakeLists.txt")))
        return 0;
    return path_is_file(cli) && path_is_file(cfg);
}

static int find_on_path(const char* name, char* out, size_t cap) {
#if defined(_WIN32)
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "where %s >nul 2>nul", name);
    if (system(cmd) == 0) {
        snprintf(out, cap, "%s", name);
        return 1;
    }
#else
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", name);
    if (system(cmd) == 0) {
        snprintf(out, cap, "%s", name);
        return 1;
    }
#endif
    return 0;
}

static int find_python(char* out, size_t cap) {
    const char* env = getenv("PYTHON");
    if (env && env[0] && path_is_file(env)) {
        snprintf(out, cap, "%s", env);
        return 1;
    }
#if defined(_WIN32)
    const char* candidates[] = { "python.exe", "python3.exe", "py.exe" };
#else
    const char* candidates[] = { "python3", "python" };
#endif
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (find_on_path(candidates[i], out, cap))
            return 1;
    }
    return 0;
}

static int toolchain_bin_has_cmake(const char* bin, char* out, size_t cap) {
    char cmake[1200];
#if defined(_WIN32)
    if (join_path(cmake, sizeof(cmake), bin, "cmake.exe") && path_is_file(cmake)) {
        snprintf(out, cap, "%s", bin);
        return 1;
    }
#else
    if (join_path(cmake, sizeof(cmake), bin, "cmake") && path_is_file(cmake)) {
        snprintf(out, cap, "%s", bin);
        return 1;
    }
#endif
    return 0;
}

static int resolve_toolchain_bin_under(const char* wrap, char* out, size_t cap) {
    char cand[1100], cmake[1200];
    if (!wrap || !wrap[0] || !path_is_dir(wrap))
        return 0;
    if (join_path(cand, sizeof(cand), wrap, "bin") &&
        toolchain_bin_has_cmake(cand, out, cap))
        return 1;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    char pattern[1200];
    snprintf(pattern, sizeof(pattern), "%s\\*", wrap);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    int found = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (fd.cFileName[0] == '.')
            continue;
        char nested[1100], nbin[1100];
        if (!join_path(nested, sizeof(nested), wrap, fd.cFileName))
            continue;
        if (!join_path(nbin, sizeof(nbin), nested, "bin"))
            continue;
        if (join_path(cmake, sizeof(cmake), nbin, "cmake.exe") && path_is_file(cmake)) {
            snprintf(out, cap, "%s", nbin);
            found = 1;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
#else
    DIR* dir = opendir(wrap);
    if (!dir)
        return 0;
    int found = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        char nested[1100], nbin[1100];
        if (!join_path(nested, sizeof(nested), wrap, ent->d_name))
            continue;
        if (!path_is_dir(nested))
            continue;
        if (!join_path(nbin, sizeof(nbin), nested, "bin"))
            continue;
        if (join_path(cmake, sizeof(cmake), nbin, "cmake") && path_is_file(cmake)) {
            snprintf(out, cap, "%s", nbin);
            found = 1;
            break;
        }
    }
    closedir(dir);
    return found;
#endif
}

/* Probe RetComM / gbarecomp shared caches (cmake-clang-v1 modular pack). */
static int resolve_shared_toolchain_bin(char* out, size_t cap) {
    char bases[4][1100];
    int n = 0;
#if defined(_WIN32)
    const char* local = getenv("LOCALAPPDATA");
    if (local && local[0] && n < 4) {
        snprintf(bases[n++], sizeof(bases[0]),
                 "%s\\retcomm\\toolchains\\cmake-clang-v1", local);
        snprintf(bases[n++], sizeof(bases[0]),
                 "%s\\gbarecomp\\toolchains\\cmake-clang-v1", local);
    }
#else
    const char* xdg = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    if (xdg && xdg[0] && n < 4) {
        snprintf(bases[n++], sizeof(bases[0]),
                 "%s/retcomm/toolchains/cmake-clang-v1", xdg);
        snprintf(bases[n++], sizeof(bases[0]),
                 "%s/gbarecomp/toolchains/cmake-clang-v1", xdg);
    } else if (home && home[0] && n < 4) {
        snprintf(bases[n++], sizeof(bases[0]),
                 "%s/.local/share/retcomm/toolchains/cmake-clang-v1", home);
        snprintf(bases[n++], sizeof(bases[0]),
                 "%s/.local/share/gbarecomp/toolchains/cmake-clang-v1", home);
    }
#endif
    for (int i = 0; i < n; ++i) {
        if (resolve_toolchain_bin_under(bases[i], out, cap))
            return 1;
    }
    return 0;
}

static int resolve_toolchain_bin(char* out, size_t cap) {
    const char* env_keys[] = {
        "GBARECOMP_TOOLCHAIN_DIR", "EMERALD_TOOLCHAIN_DIR",
        "RETCOMM_TOOLCHAIN_DIR", "TOOLCHAIN_DIR", "BPE_TOOLCHAIN_DIR", NULL};
    for (int i = 0; env_keys[i]; ++i) {
        const char* e = getenv(env_keys[i]);
        if (e && e[0] && resolve_toolchain_bin_under(e, out, cap))
            return 1;
    }
    if (g_project_root[0]) {
        char wrap[1100];
        if (join_path(wrap, sizeof(wrap), g_project_root, "toolchain") &&
            resolve_toolchain_bin_under(wrap, out, cap))
            return 1;
    }
    return resolve_shared_toolchain_bin(out, cap);
}

static void activate_toolchain_path(void) {
    g_toolchain_bin[0] = '\0';
    if (!resolve_toolchain_bin(g_toolchain_bin, sizeof(g_toolchain_bin)))
        return;
    const char* old = getenv("PATH");
#if defined(_WIN32)
    char neu[8192];
    snprintf(neu, sizeof(neu), "%s%s%s", g_toolchain_bin, old ? ";" : "",
             old ? old : "");
    _putenv_s("PATH", neu);
#else
    char neu[8192];
    snprintf(neu, sizeof(neu), "%s%s%s", g_toolchain_bin, old ? ":" : "",
             old ? old : "");
    setenv("PATH", neu, 1);
#endif
}

static int find_cmake(char* out, size_t cap) {
    const char* env = getenv("CMAKE");
    if (env && env[0] && path_is_file(env)) {
        snprintf(out, cap, "%s", env);
        return 1;
    }
    char tc[1100], cand[1200];
    if (resolve_toolchain_bin(tc, sizeof(tc))) {
#if defined(_WIN32)
        if (join_path(cand, sizeof(cand), tc, "cmake.exe") && path_is_file(cand)) {
            snprintf(out, cap, "%s", cand);
            return 1;
        }
#else
        if (join_path(cand, sizeof(cand), tc, "cmake") && path_is_file(cand)) {
            snprintf(out, cap, "%s", cand);
            return 1;
        }
#endif
    }
#if defined(_WIN32)
    return find_on_path("cmake.exe", out, cap);
#else
    return find_on_path("cmake", out, cap);
#endif
}

static int discover_project_root(char* out, size_t cap) {
    const char* env_name =
        cfg_or(g_cfg->project_root_env, "GBARECOMP_PROJECT_ROOT");
    const char* env = getenv(env_name);
    if (env && env[0] && looks_like_project_root(env)) {
        snprintf(out, cap, "%s", env);
        return 1;
    }

    char start[1024];
#if defined(_WIN32)
    if (!GetCurrentDirectoryA((DWORD)sizeof(start), start))
        start[0] = '\0';
#else
    if (!getcwd(start, sizeof(start)))
        start[0] = '\0';
#endif

    char cur[1024];
    snprintf(cur, sizeof(cur), "%s", start[0] ? start : ".");
    for (int i = 0; i < 8; ++i) {
        if (looks_like_project_root(cur)) {
            snprintf(out, cap, "%s", cur);
            return 1;
        }
        char parent[1024];
        if (!dirname_copy(parent, sizeof(parent), cur))
            break;
        if (strcmp(parent, cur) == 0)
            break;
        snprintf(cur, sizeof(cur), "%s", parent);
    }
    return 0;
}

static int resolve_build_paths(void) {
    const char* env_name =
        cfg_or(g_cfg->build_dir_env, "GBARECOMP_BUILD_DIR");
    const char* env = getenv(env_name);
    if (env && env[0]) {
        snprintf(g_build_dir, sizeof(g_build_dir), "%s", env);
    } else {
        const char* names[] = {
            cfg_or(g_cfg->build_dir_name, "build"),
            "build",
            "build-ci",
            "build-release",
        };
        int found = 0;
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            if (!names[i] || !names[i][0])
                continue;
            char cand[1100];
            if (!join_path(cand, sizeof(cand), g_project_root, names[i]))
                continue;
            if (path_is_dir(cand)) {
                snprintf(g_build_dir, sizeof(g_build_dir), "%s", cand);
                found = 1;
                break;
            }
        }
        if (!found) {
            const char* prefer = cfg_or(g_cfg->build_dir_name, "build");
            if (!join_path(g_build_dir, sizeof(g_build_dir), g_project_root,
                           prefer))
                return 0;
        }
    }

    char exe_name[300];
#if defined(_WIN32)
    snprintf(exe_name, sizeof(exe_name), "%s.exe", g_cfg->exe_basename);
#else
    snprintf(exe_name, sizeof(exe_name), "%s", g_cfg->exe_basename);
#endif
    return join_path(g_exe_path, sizeof(g_exe_path), g_build_dir, exe_name);
}

int gbarecomp_codegen_host_sources_missing(
    const GbarecompCodegenHostConfig* cfg) {
    if (!cfg || !cfg->cmake_target || !cfg->exe_basename)
        return 0;
    g_cfg = cfg;
    if (!g_ready && !discover_project_root(g_project_root, sizeof(g_project_root)))
        return 0;
    char marker_rel[512];
    if (cfg->gen_marker_relpath && cfg->gen_marker_relpath[0]) {
        snprintf(marker_rel, sizeof(marker_rel), "%s", cfg->gen_marker_relpath);
    } else if (cfg->out_dir && cfg->out_dir[0]) {
        snprintf(marker_rel, sizeof(marker_rel), "%s/dispatch_table.cpp",
                 cfg->out_dir);
    } else {
        return 1;
    }
    char marker[1100];
    if (!join_path(marker, sizeof(marker), g_project_root, marker_rel))
        return 1;
    return !path_is_file(marker);
}

static int json_get_string(const char* line, const char* key, char* out,
                           size_t out_cap) {
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* p = strstr(line, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_cap) {
        if (*p == '\\' && p[1]) {
            ++p;
            out[i++] = *p++;
            continue;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

static int json_get_number(const char* line, const char* key, double* out) {
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(line, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ') ++p;
    char* end = NULL;
    double v = strtod(p, &end);
    if (end == p) return 0;
    *out = v;
    return 1;
}

static void remember_toolchain_bin_from_json(const char* line) {
    char bin[1100] = "";
    if (!json_get_string(line, "toolchain_bin", bin, sizeof(bin)) || !bin[0])
        return;
    /* ensure-toolchain returns …/bin; env aliases want the pack root. */
    char parent[1100];
    if (!dirname_copy(parent, sizeof(parent), bin))
        return;
#if defined(_WIN32)
    _putenv_s("GBARECOMP_TOOLCHAIN_DIR", parent);
    _putenv_s("RETCOMM_TOOLCHAIN_DIR", parent);
#else
    setenv("GBARECOMP_TOOLCHAIN_DIR", parent, 1);
    setenv("RETCOMM_TOOLCHAIN_DIR", parent, 1);
#endif
}

static void handle_progress_line(const char* line,
                                 RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx) {
    if (!line || line[0] != '{') return;
    char event[64] = "";
    json_get_string(line, "event", event, sizeof(event));
    if (strcmp(event, "result") == 0)
        remember_toolchain_bin_from_json(line);
    if (!on_progress) return;
    if (strcmp(event, "phase") == 0) {
        char message[240] = "";
        char phase[64] = "";
        double pct = -1.0;
        json_get_string(line, "message", message, sizeof(message));
        json_get_string(line, "phase", phase, sizeof(phase));
        if (!json_get_number(line, "pct", &pct))
            pct = -1.0;
        if (!message[0] && phase[0])
            snprintf(message, sizeof(message), "%s", phase);
        on_progress(progress_ctx, (float)pct, message[0] ? message : NULL);
    } else if (strcmp(event, "log") == 0 || strcmp(event, "error") == 0) {
        char message[240] = "";
        if (json_get_string(line, "message", message, sizeof(message)))
            on_progress(progress_ctx, -1.0f, message);
    }
}

#if defined(_WIN32)
static int run_generate_win(const char* rom,
                            RecompLauncherCPrepareProgressFn on_progress,
                            void* progress_ctx, char* err_msg, size_t err_cap) {
    char cmdline[4096];
    char sha_arg[160] = "";
    char bios_arg[640] = "";
    if (g_cfg->expected_sha1 && g_cfg->expected_sha1[0])
        snprintf(sha_arg, sizeof(sha_arg), " --expected-sha1 %s",
                 g_cfg->expected_sha1);
    if (g_has_bios)
        snprintf(bios_arg, sizeof(bios_arg), " --bios \"%s\"", g_bios);

    snprintf(cmdline, sizeof(cmdline),
             "\"%s\" \"%s\" generate --project-root \"%s\" --rom \"%s\" "
             "--config \"%s\" --out-dir \"%s\"%s%s --json-progress",
             g_python, g_cli_path, g_project_root, rom, g_config, g_out_dir,
             bios_arg, sha_arg);

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        snprintf(err_msg, err_cap, "CreatePipe failed.");
        return 0;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    char mutable_cmd[4096];
    snprintf(mutable_cmd, sizeof(mutable_cmd), "%s", cmdline);
    if (!CreateProcessA(NULL, mutable_cmd, NULL, NULL, TRUE, 0, NULL,
                        g_project_root, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        snprintf(err_msg, err_cap, "Failed to spawn gbarecomp generate.");
        return 0;
    }
    CloseHandle(wr);

    char buf[512];
    char line[1024];
    size_t line_len = 0;
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, NULL) && n > 0) {
        for (DWORD i = 0; i < n; ++i) {
            char c = buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[line_len] = '\0';
                handle_progress_line(line, on_progress, progress_ctx);
                line_len = 0;
                continue;
            }
            if (line_len + 1 < sizeof(line))
                line[line_len++] = c;
        }
    }
    if (line_len) {
        line[line_len] = '\0';
        handle_progress_line(line, on_progress, progress_ctx);
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code == 0) return 1;
    if (code == 3)
        snprintf(err_msg, err_cap, "ROM verification failed (wrong dump).");
    else
        snprintf(err_msg, err_cap, "gbarecomp generate failed (exit %lu).",
                 (unsigned long)code);
    return 0;
}
#else
static int run_generate_posix(const char* rom,
                              RecompLauncherCPrepareProgressFn on_progress,
                              void* progress_ctx, char* err_msg,
                              size_t err_cap) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        snprintf(err_msg, err_cap, "pipe() failed: %s", strerror(errno));
        return 0;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    char* argv[40];
    int argc = 0;
    argv[argc++] = g_python;
    argv[argc++] = g_cli_path;
    argv[argc++] = "generate";
    argv[argc++] = "--project-root";
    argv[argc++] = g_project_root;
    argv[argc++] = "--rom";
    argv[argc++] = (char*)rom;
    argv[argc++] = "--config";
    argv[argc++] = g_config;
    argv[argc++] = "--out-dir";
    argv[argc++] = g_out_dir;
    if (g_has_bios) {
        argv[argc++] = "--bios";
        argv[argc++] = g_bios;
    }
    if (g_cfg->expected_sha1 && g_cfg->expected_sha1[0]) {
        argv[argc++] = "--expected-sha1";
        argv[argc++] = (char*)g_cfg->expected_sha1;
    }
    argv[argc++] = "--json-progress";
    argv[argc] = NULL;

    pid_t pid = 0;
    int rc = posix_spawnp(&pid, g_python, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if (rc != 0) {
        close(pipefd[0]);
        snprintf(err_msg, err_cap, "Failed to spawn gbarecomp generate: %s",
                 strerror(rc));
        return 0;
    }

    FILE* out = fdopen(pipefd[0], "r");
    if (!out) {
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        snprintf(err_msg, err_cap, "fdopen failed.");
        return 0;
    }
    char line[1024];
    while (fgets(line, sizeof(line), out)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        handle_progress_line(line, on_progress, progress_ctx);
    }
    fclose(out);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        snprintf(err_msg, err_cap, "waitpid failed: %s", strerror(errno));
        return 0;
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    if (code == 0) return 1;
    if (code == 3)
        snprintf(err_msg, err_cap, "ROM verification failed (wrong dump).");
    else
        snprintf(err_msg, err_cap, "gbarecomp generate failed (exit %d).",
                 code);
    return 0;
}
#endif

static int host_toolchain_is_ready(void) {
    if (!g_ready)
        return 0;
    activate_toolchain_path();
    return find_cmake(g_cmake, sizeof(g_cmake)) ? 1 : 0;
}

static int host_ensure_toolchain_with_progress(
    int download, const char* zip_path, char* err_msg, size_t err_cap,
    RecompLauncherCPrepareProgressFn on_progress, void* progress_ctx) {
    if (!g_ready) {
        snprintf(err_msg, err_cap, "Local codegen tools are not available.");
        return 0;
    }
    activate_toolchain_path();
    if (find_cmake(g_cmake, sizeof(g_cmake)))
        return 1;

    if (on_progress)
        on_progress(progress_ctx, 0.05f,
                    zip_path && zip_path[0]
                        ? "Installing toolchain from zip…"
                        : (download ? "Downloading portable cmake/clang…"
                                    : "Looking for portable toolchain…"));

#if defined(_WIN32)
    char cmdline[4096];
    if (zip_path && zip_path[0]) {
        snprintf(cmdline, sizeof(cmdline),
                 "\"%s\" \"%s\" ensure-toolchain --project-root \"%s\" "
                 "--from-zip \"%s\" --json-progress",
                 g_python, g_cli_path, g_project_root, zip_path);
    } else if (download) {
        snprintf(cmdline, sizeof(cmdline),
                 "\"%s\" \"%s\" ensure-toolchain --project-root \"%s\" "
                 "--json-progress",
                 g_python, g_cli_path, g_project_root);
    } else {
        snprintf(cmdline, sizeof(cmdline),
                 "\"%s\" \"%s\" ensure-toolchain --project-root \"%s\" "
                 "--no-download --json-progress",
                 g_python, g_cli_path, g_project_root);
    }
    {
        SECURITY_ATTRIBUTES sa;
        memset(&sa, 0, sizeof(sa));
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE rd = NULL, wr = NULL;
        if (!CreatePipe(&rd, &wr, &sa, 0)) {
            snprintf(err_msg, err_cap, "CreatePipe failed.");
            return 0;
        }
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        memset(&pi, 0, sizeof(pi));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = wr;
        si.hStdError = wr;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        char cmd_mutable[4096];
        snprintf(cmd_mutable, sizeof(cmd_mutable), "%s", cmdline);
        if (!CreateProcessA(NULL, cmd_mutable, NULL, NULL, TRUE, 0, NULL,
                            g_project_root, &si, &pi)) {
            CloseHandle(rd);
            CloseHandle(wr);
            snprintf(err_msg, err_cap, "Failed to spawn ensure-toolchain.");
            return 0;
        }
        CloseHandle(wr);
        char buf[512];
        char line[1024];
        size_t line_len = 0;
        DWORD nread = 0;
        while (ReadFile(rd, buf, sizeof(buf), &nread, NULL) && nread > 0) {
            for (DWORD i = 0; i < nread; ++i) {
                char c = buf[i];
                if (c == '\r') continue;
                if (c == '\n') {
                    line[line_len] = '\0';
                    handle_progress_line(line, on_progress, progress_ctx);
                    line_len = 0;
                    continue;
                }
                if (line_len + 1 < sizeof(line))
                    line[line_len++] = c;
            }
        }
        if (line_len) {
            line[line_len] = '\0';
            handle_progress_line(line, on_progress, progress_ctx);
        }
        CloseHandle(rd);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (code != 0) {
            snprintf(err_msg, err_cap, "ensure-toolchain failed (exit %lu).",
                     (unsigned long)code);
            return 0;
        }
    }
#else
    {
        char* argv[16];
        int argc = 0;
        char zip_storage[1100];
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            snprintf(err_msg, err_cap, "pipe() failed: %s", strerror(errno));
            return 0;
        }
        argv[argc++] = g_python;
        argv[argc++] = g_cli_path;
        argv[argc++] = "ensure-toolchain";
        argv[argc++] = "--project-root";
        argv[argc++] = g_project_root;
        if (zip_path && zip_path[0]) {
            snprintf(zip_storage, sizeof(zip_storage), "%s", zip_path);
            argv[argc++] = "--from-zip";
            argv[argc++] = zip_storage;
        } else if (!download) {
            argv[argc++] = "--no-download";
        }
        argv[argc++] = "--json-progress";
        argv[argc] = NULL;

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipefd[1]);

        pid_t pid = 0;
        int rc = posix_spawnp(&pid, g_python, &actions, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
        close(pipefd[1]);
        if (rc != 0) {
            close(pipefd[0]);
            snprintf(err_msg, err_cap, "Failed to spawn ensure-toolchain: %s",
                     strerror(rc));
            return 0;
        }
        FILE* out = fdopen(pipefd[0], "r");
        if (!out) {
            close(pipefd[0]);
            waitpid(pid, NULL, 0);
            snprintf(err_msg, err_cap, "fdopen failed.");
            return 0;
        }
        char line[1024];
        while (fgets(line, sizeof(line), out)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                line[--n] = '\0';
            handle_progress_line(line, on_progress, progress_ctx);
        }
        fclose(out);
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) {
            snprintf(err_msg, err_cap, "waitpid failed: %s", strerror(errno));
            return 0;
        }
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        if (code != 0) {
            snprintf(err_msg, err_cap, "ensure-toolchain failed (exit %d).",
                     code);
            return 0;
        }
    }
#endif

    activate_toolchain_path();
    if (find_cmake(g_cmake, sizeof(g_cmake)))
        return 1;
    snprintf(err_msg, err_cap,
             "Toolchain install finished but cmake was not found. "
             "Use a cmake-clang-v1 pack, set GBARECOMP_TOOLCHAIN_DIR / "
             "RETCOMM_TOOLCHAIN_DIR, or install cmake on PATH.");
    return 0;
}

static int host_ensure_toolchain(RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx, char* err_msg,
                                 size_t err_cap) {
    return host_ensure_toolchain_with_progress(1, NULL, err_msg, err_cap,
                                               on_progress, progress_ctx);
}

static int host_prepare_generate(const char* source_path, char* out_path,
                                 size_t out_cap, char* err_msg, size_t err_cap,
                                 RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx) {
    if (!g_ready) {
        snprintf(err_msg, err_cap, "Local codegen tools are not available.");
        return 0;
    }
    if (!source_path || !source_path[0]) {
        snprintf(err_msg, err_cap, "No ROM selected.");
        return 0;
    }
    activate_toolchain_path();
    if (on_progress)
        on_progress(progress_ctx, 0.02f, "Starting gbarecomp generate…");

#if defined(_WIN32)
    if (!run_generate_win(source_path, on_progress, progress_ctx, err_msg,
                          err_cap))
        return 0;
#else
    if (!run_generate_posix(source_path, on_progress, progress_ctx, err_msg,
                            err_cap))
        return 0;
#endif

    snprintf(out_path, out_cap, "%s", source_path);
    if (on_progress)
        on_progress(progress_ctx, 1.0f, "Generate complete");
    return 1;
}

#if defined(_WIN32)
static void bat_write_set(FILE* f, const char* name, const char* value) {
    fprintf(f, "set \"%s=", name);
    for (const char* p = value; *p; ++p) {
        if (*p == '%')
            fputc('%', f);
        fputc(*p, f);
    }
    fprintf(f, "\"\r\n");
}

static int write_windows_deferred_rebuild_helper(char* err_msg, size_t err_cap) {
#if defined(_WIN32)
    CreateDirectoryA(g_build_dir, NULL);
#endif
    if (!join_path(g_helper_path, sizeof(g_helper_path), g_build_dir,
                   "recomp_deferred_rebuild.cmd")) {
        snprintf(err_msg, err_cap, "Failed to form helper path.");
        return 0;
    }
    FILE* f = fopen(g_helper_path, "wb");
    if (!f) {
        snprintf(err_msg, err_cap, "Failed to write rebuild helper: %s",
                 g_helper_path);
        return 0;
    }
    char pid_buf[32];
    snprintf(pid_buf, sizeof(pid_buf), "%lu",
             (unsigned long)GetCurrentProcessId());
    fprintf(f, "@echo off\r\n");
    fprintf(f, "setlocal EnableExtensions\r\n");
    fprintf(f, "title %s - rebuilding\r\n", g_display);
    bat_write_set(f, "PARENT_PID", pid_buf);
    bat_write_set(f, "PYTHON", g_python);
    bat_write_set(f, "CLI", g_cli_path);
    bat_write_set(f, "BUILD_DIR", g_build_dir);
    bat_write_set(f, "EXE", g_exe_path);
    bat_write_set(f, "ROOT", g_project_root);
    bat_write_set(f, "TARGET", g_cmake_target);
    bat_write_set(f, "DISPLAY", g_display);
    if (g_toolchain_bin[0])
        bat_write_set(f, "TC_BIN", g_toolchain_bin);
    fprintf(f,
            "echo Waiting for %%DISPLAY%% to exit...\r\n"
            ":waitloop\r\n"
            "tasklist /FI \"PID eq %%PARENT_PID%%\" 2>NUL | "
            "findstr /I \"%%PARENT_PID%%\" >NUL\r\n"
            "if not errorlevel 1 (\r\n"
            "  ping -n 2 127.0.0.1 >NUL\r\n"
            "  goto waitloop\r\n"
            ")\r\n"
            "echo Ensuring toolchain...\r\n"
            "cd /d \"%%ROOT%%\"\r\n"
            "if defined TC_BIN set \"PATH=%%TC_BIN%%;%%PATH%%\"\r\n"
            "\"%%PYTHON%%\" \"%%CLI%%\" ensure-toolchain --project-root \"%%ROOT%%\"\r\n"
            "if errorlevel 1 (\r\n"
            "  echo.\r\n"
            "  echo Toolchain missing. Download cmake-clang-v1 or set\r\n"
            "  echo GBARECOMP_TOOLCHAIN_DIR / RETCOMM_TOOLCHAIN_DIR.\r\n"
            "  pause\r\n"
            "  exit /b 1\r\n"
            ")\r\n"
            "echo Building...\r\n"
            "\"%%PYTHON%%\" \"%%CLI%%\" rebuild --project-root \"%%ROOT%%\" "
            "--build-dir \"%%BUILD_DIR%%\" --target \"%%TARGET%%\" "
            "--exe-basename \"%%TARGET%%\" "
            "--prune-after build-intermediates\r\n"
            "if errorlevel 1 (\r\n"
            "  echo.\r\n"
            "  echo Build failed. Fix the errors above, then rebuild manually.\r\n"
            "  pause\r\n"
            "  exit /b 1\r\n"
            ")\r\n"
            "echo Starting %%DISPLAY%%...\r\n"
            "start \"\" /D \"%%ROOT%%\" \"%%EXE%%\" --launcher\r\n"
            "endlocal\r\n");
    fclose(f);
    return 1;
}
#else
static int run_rebuild_cli_posix(RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx, char* err_msg,
                                 size_t err_cap) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        snprintf(err_msg, err_cap, "pipe() failed: %s", strerror(errno));
        return 0;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    char* argv[] = {
        g_python,
        g_cli_path,
        "rebuild",
        "--project-root", g_project_root,
        "--build-dir", g_build_dir,
        "--target", g_cmake_target,
        "--exe-basename", g_cmake_target,
        "--prune-after", "toolchain,build-intermediates",
        "--json-progress",
        NULL
    };

    pid_t pid = 0;
    int rc = posix_spawnp(&pid, g_python, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if (rc != 0) {
        close(pipefd[0]);
        snprintf(err_msg, err_cap, "Failed to spawn gbarecomp rebuild: %s",
                 strerror(rc));
        return 0;
    }

    FILE* out = fdopen(pipefd[0], "r");
    if (!out) {
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        snprintf(err_msg, err_cap, "fdopen failed.");
        return 0;
    }
    char line[1024];
    while (fgets(line, sizeof(line), out)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        handle_progress_line(line, on_progress, progress_ctx);
    }
    fclose(out);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        snprintf(err_msg, err_cap, "waitpid failed: %s", strerror(errno));
        return 0;
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    if (code == 0) return 1;
    snprintf(err_msg, err_cap, "gbarecomp rebuild failed (exit %d).", code);
    return 0;
}
#endif

static int host_rebuild_game(const char* rom_path, char* out_exe_path,
                             size_t out_cap, char* err_msg, size_t err_cap,
                             RecompLauncherCPrepareProgressFn on_progress,
                             void* progress_ctx) {
    (void)rom_path;
    g_relaunch_is_helper = 0;
    if (!g_ready || !g_build_dir[0]) {
        snprintf(err_msg, err_cap, "CMake build environment is not available.");
        return 0;
    }

    if (!host_ensure_toolchain(on_progress, progress_ctx, err_msg, err_cap))
        return 0;
    activate_toolchain_path();
    if (!g_cmake[0])
        find_cmake(g_cmake, sizeof(g_cmake));

#if defined(_WIN32)
    if (on_progress)
        on_progress(progress_ctx, 0.4f,
                    "Scheduling Windows rebuild after exit…");
    if (!write_windows_deferred_rebuild_helper(err_msg, err_cap))
        return 0;
    g_relaunch_is_helper = 1;
    snprintf(out_exe_path, out_cap, "%s", g_helper_path);
    if (on_progress)
        on_progress(progress_ctx, 1.0f,
                    "Exiting so Windows can rebuild safely…");
    return 1;
#else
    if (on_progress)
        on_progress(progress_ctx, 0.05f, "Starting rebuild (cmake)…");
    if (!run_rebuild_cli_posix(on_progress, progress_ctx, err_msg, err_cap))
        return 0;
    if (!path_is_file(g_exe_path)) {
        snprintf(err_msg, err_cap, "Build succeeded but binary missing: %s",
                 g_exe_path);
        return 0;
    }
    snprintf(out_exe_path, out_cap, "%s", g_exe_path);
    if (on_progress)
        on_progress(progress_ctx, 1.0f, "Build complete");
    return 1;
#endif
}

static int write_line_file(const char* path, const char* line) {
    FILE* f;
    if (!path || !path[0])
        return 0;
    if (!line || !line[0]) {
        remove(path);
        return 1;
    }
    f = fopen(path, "w");
    if (!f)
        return 0;
    fprintf(f, "%s\n", line);
    fclose(f);
    return 1;
}

static int read_line_file(const char* path, char* out, size_t cap) {
    FILE* f;
    size_t n;
    if (!path || !path[0] || !out || cap == 0)
        return 0;
    out[0] = '\0';
    f = fopen(path, "r");
    if (!f)
        return 0;
    if (!fgets(out, (int)cap, f)) {
        fclose(f);
        out[0] = '\0';
        return 0;
    }
    fclose(f);
    n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                     out[n - 1] == ' ' || out[n - 1] == '\t'))
        out[--n] = '\0';
    return out[0] != '\0';
}

/* Sidecars are loaded next to argv[0] (see launcher_seam state_path). The
 * setup host often lives at the zip root while the rebuilt binary is under
 * build/ — write beside the game binary, not only cwd. */
static void write_sidecar_near_exe(const char* near_exe, const char* name,
                                   const char* value) {
    char dir[1100], path[1200];
    if (!near_exe || !near_exe[0] || !name || !name[0])
        return;
    if (!dirname_copy(dir, sizeof(dir), near_exe))
        return;
    if (!join_path(path, sizeof(path), dir, name))
        return;
    write_line_file(path, value ? value : "");
}

static int host_persist_setup(void* ctx, const char* rom_path,
                              const char* bios_path) {
    char path[1200];
    (void)ctx;
    if (g_project_root[0] &&
        join_path(path, sizeof(path), g_project_root, "bios.cfg"))
        write_line_file(path, (bios_path && bios_path[0]) ? bios_path : "");
    write_line_file("bios.cfg", (bios_path && bios_path[0]) ? bios_path : "");
    if (g_exe_path[0])
        write_sidecar_near_exe(g_exe_path, "bios.cfg",
                               (bios_path && bios_path[0]) ? bios_path : "");
    if (rom_path && rom_path[0]) {
        if (g_project_root[0] &&
            join_path(path, sizeof(path), g_project_root, "rom.cfg"))
            write_line_file(path, rom_path);
        write_line_file("rom.cfg", rom_path);
        if (g_exe_path[0])
            write_sidecar_near_exe(g_exe_path, "rom.cfg", rom_path);
    }
    return 0;
}

static void persist_relaunch_sidecars(const char* near_exe,
                                      const char* rom_path) {
    char bios_line[1024];
    char project_sidecar[1200];

    if (rom_path && rom_path[0]) {
        write_sidecar_near_exe(near_exe, "rom.cfg", rom_path);
        write_line_file("rom.cfg", rom_path);
        if (g_project_root[0] &&
            join_path(project_sidecar, sizeof(project_sidecar), g_project_root,
                      "rom.cfg"))
            write_line_file(project_sidecar, rom_path);
    }

    bios_line[0] = '\0';
    if (!read_line_file("bios.cfg", bios_line, sizeof(bios_line)) &&
        g_project_root[0] &&
        join_path(project_sidecar, sizeof(project_sidecar), g_project_root,
                  "bios.cfg"))
        read_line_file(project_sidecar, bios_line, sizeof(bios_line));
    if ((!bios_line[0] || !path_is_file(bios_line)) && g_has_bios && g_bios[0])
        snprintf(bios_line, sizeof(bios_line), "%s", g_bios);
    if (bios_line[0]) {
        write_sidecar_near_exe(near_exe, "bios.cfg", bios_line);
        write_line_file("bios.cfg", bios_line);
        if (g_project_root[0] &&
            join_path(project_sidecar, sizeof(project_sidecar), g_project_root,
                      "bios.cfg"))
            write_line_file(project_sidecar, bios_line);
    }
}

void gbarecomp_codegen_host_relaunch_or_exit(const char* rom_path) {
    char exe[512];
    const char* near_exe;
    if (!recomp_launcher_relaunch_exe(exe, sizeof(exe)) || !exe[0]) {
        fprintf(stderr, "gbarecomp-codegen: relaunch requested but no path\n");
        exit(1);
    }
    /* Prefer the final game binary (build/<exe>) over a Windows helper bat. */
    near_exe = g_exe_path[0] ? g_exe_path : exe;
    persist_relaunch_sidecars(near_exe, rom_path);

#if defined(_WIN32)
    {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        char cmd[1536];
        DWORD flags = 0;
        memset(&si, 0, sizeof(si));
        memset(&pi, 0, sizeof(pi));
        si.cb = sizeof(si);
        if (g_relaunch_is_helper) {
            fprintf(stderr,
                    "gbarecomp-codegen: starting deferred rebuild helper\n");
            snprintf(cmd, sizeof(cmd), "cmd.exe /C \"%s\"", exe);
            flags = CREATE_NEW_CONSOLE;
        } else {
            fprintf(stderr, "gbarecomp-codegen: relaunching %s\n", exe);
            snprintf(cmd, sizeof(cmd), "\"%s\" --launcher", exe);
        }
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, flags, NULL,
                            g_project_root, &si, &pi)) {
            fprintf(stderr, "gbarecomp-codegen: CreateProcess failed\n");
            exit(1);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ExitProcess(0);
    }
#else
    {
        if (g_project_root[0] && chdir(g_project_root) != 0) {
            fprintf(stderr, "gbarecomp-codegen: chdir(%s) failed: %s\n",
                    g_project_root, strerror(errno));
        }
        fprintf(stderr, "gbarecomp-codegen: relaunching %s\n", exe);
        char* args[] = { exe, "--launcher", NULL };
        execv(exe, args);
        perror("gbarecomp-codegen: execv failed");
        exit(1);
    }
#endif
}

void gbarecomp_codegen_host_apply(RecompLauncherCGameInfo* gi,
                                   const GbarecompCodegenHostConfig* cfg) {
    if (!gi || !cfg || !cfg->cmake_target || !cfg->exe_basename)
        return;

    g_cfg = cfg;
    g_ready = 0;
    g_relaunch_is_helper = 0;
    g_project_root[0] = '\0';
    g_cli_path[0] = '\0';
    g_python[0] = '\0';
    g_cmake[0] = '\0';
    g_build_dir[0] = '\0';
    g_toolchain_bin[0] = '\0';
    g_exe_path[0] = '\0';
    g_helper_path[0] = '\0';

    snprintf(g_display, sizeof(g_display), "%s",
             cfg_or(cfg->display_name, "Game"));
    if (!cfg->config || !cfg->config[0] || !cfg->out_dir || !cfg->out_dir[0])
        return;
    snprintf(g_config, sizeof(g_config), "%s", cfg->config);
    snprintf(g_out_dir, sizeof(g_out_dir), "%s", cfg->out_dir);
    snprintf(g_cmake_target, sizeof(g_cmake_target), "%s", cfg->cmake_target);
    g_has_bios = 0;
    g_bios[0] = '\0';

    if (!discover_project_root(g_project_root, sizeof(g_project_root)))
        return;
    activate_toolchain_path();
    if (cfg->bios_relpath && cfg->bios_relpath[0]) {
        char bios_abs[1100];
        if (join_path(bios_abs, sizeof(bios_abs), g_project_root,
                      cfg->bios_relpath) &&
            path_is_file(bios_abs)) {
            snprintf(g_bios, sizeof(g_bios), "%s", bios_abs);
            g_has_bios = 1;
        }
    }
    if (!join_path(g_cli_path, sizeof(g_cli_path), g_project_root,
                   cfg_or(cfg->gbarecomp_cli_relpath,
                          "gbarecomp/gbarecomp_cli.py")))
        return;
    if (!path_is_file(g_cli_path))
        return;
    if (!find_python(g_python, sizeof(g_python)))
        return;

    g_ready = 1;
    activate_toolchain_path();
    gi->persist_setup = host_persist_setup;
    gi->persist_setup_ctx = NULL;
    gi->prepare_with_progress = host_prepare_generate;
    gi->prepare_use_selected_rom = 1;
    gi->prepare_section_title = "Generate C sources & rebuild";
    gi->prepare_busy_status = "Generating sources…";
    gi->prepare_success_status = "Sources ready — building…";

    /* Rebuild is offered whenever the build tree can be formed; wizard page 0
     * installs cmake-clang-v1 before Generate & rebuild (BPE modular flow). */
    const int can_rebuild = resolve_build_paths();
    if (can_rebuild) {
        gi->prepare_disc_label = "Generate & rebuild…";
#if defined(_WIN32)
        gi->prepare_disc_note =
            cfg->prepare_note_windows
                ? cfg->prepare_note_windows
                : "Regenerates sources with the local gbarecomp SDK, then "
                  "quits and rebuilds via a helper so the running .exe is not "
                  "locked. You must legally own this ROM.";
        gi->rebuild_busy_status = "Scheduling rebuild…";
        gi->rebuild_success_status =
            "Exiting for Windows rebuild — a console will finish the build…";
#else
        gi->prepare_disc_note =
            cfg->prepare_note
                ? cfg->prepare_note
                : "Regenerates sources with the local gbarecomp SDK, then "
                  "runs cmake --build and restarts into the new binary. You "
                  "must legally own this ROM.";
        gi->rebuild_busy_status = "Building game…";
        gi->rebuild_success_status = "Build complete — restarting…";
#endif
        gi->rebuild_with_progress = host_rebuild_game;
        gi->rebuild_after_prepare = 1;
        gi->relaunch_after_rebuild = 1;
        gi->setup_needs_toolchain = 1;
        gi->toolchain_is_ready = host_toolchain_is_ready;
        gi->ensure_toolchain_with_progress = host_ensure_toolchain_with_progress;
    } else {
        gi->prepare_disc_label = "Generate sources…";
        gi->prepare_disc_note =
            cfg->prepare_note_no_cmake
                ? cfg->prepare_note_no_cmake
                : "Regenerates sources with the local gbarecomp SDK. "
                  "Build dir could not be resolved — rebuild manually, then "
                  "relaunch.";
        gi->prepare_success_status =
            "Sources generated. Rebuild manually, then relaunch.";
    }

    const char* force_env =
        cfg_or(cfg->force_setup_env, "GBARECOMP_FORCE_SETUP");
    const char* force = getenv(force_env);
    if (gbarecomp_codegen_host_sources_missing(cfg) ||
        (force && force[0] && force[0] != '0')) {
        gi->needs_setup = 1;
        gi->prepare_required_before_continue = 1;
    }
}
