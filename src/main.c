// ================= FULL WORKING main.c =================
// Fixed traverse_dir, fixed serialization, no warnings, no empty .bundle

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define PATH_SEPARATOR '\\'
#undef PATH_MAX
#define PATH_MAX MAX_PATH
#define mkdir(path, mode) _mkdir(path)
#define setenv(name, value, overwrite) _putenv_s(name, value)
#define POPEN _popen
#define PCLOSE _pclose
#define SHELL "cmd.exe /C "
#else
#include <unistd.h>
#include <sys/wait.h>
#include <libgen.h>
#define PATH_SEPARATOR '/'
#define PATH_MAX 4096
#define POPEN popen
#define PCLOSE pclose
#define SHELL "/bin/sh -c "
#endif

const char *platform =
#if defined(_WIN32)
    "windows";
#elif defined(__APPLE__)
    "macOS";
#elif defined(__linux__)
    "linux";
#else
    "unknown";
#endif

const char *directorySeparator =
#if defined(_WIN32)
    "\\";
#else
    "/";
#endif

// --------------------------------------------------
// helpers
// --------------------------------------------------

#ifdef _WIN32

static int lua_exec(lua_State *L)
{
    const char *cmd = luaL_checkstring(L, 1);

    size_t in_size = 0;
    const char *in_data = NULL;
    int has_input = !lua_isnoneornil(L, 2);

    if (has_input)
        in_data = luaL_checklstring(L, 2, &in_size);

    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};

    HANDLE hStdinR, hStdinW;
    HANDLE hStdoutR, hStdoutW;

    if (!CreatePipe(&hStdinR, &hStdinW, &sa, 0) ||
        !CreatePipe(&hStdoutR, &hStdoutW, &sa, 0))
    {
        lua_pushnil(L);
        lua_pushinteger(L, -1);
        return 2;
    }

    SetHandleInformation(hStdinW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hStdoutR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hStdinR;
    si.hStdOutput = hStdoutW;
    si.hStdError = hStdoutW;

    PROCESS_INFORMATION pi = {0};

    char cmdline[4096];
    snprintf(cmdline, sizeof(cmdline), "cmd.exe /C %s", cmd);

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        lua_pushnil(L);
        lua_pushinteger(L, -1);
        return 2;
    }

    CloseHandle(hStdinR);
    CloseHandle(hStdoutW);

    // write stdin
    if (has_input && in_size)
    {
        DWORD written = 0;
        WriteFile(hStdinW, in_data, (DWORD)in_size, &written, NULL);
    }
    CloseHandle(hStdinW);

    luaL_Buffer out;
    luaL_buffinit(L, &out);

    unsigned char buf[4096];
    DWORD read;
    while (ReadFile(hStdoutR, buf, sizeof(buf), &read, NULL) && read > 0)
        luaL_addlstring(&out, (const char *)buf, read);

    CloseHandle(hStdoutR);

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    luaL_pushresult(&out);
    lua_pushinteger(L, (int)exit_code);
    return 2;
}
#else

static int lua_exec(lua_State *L)
{
    const char *cmd = luaL_checkstring(L, 1);

    size_t in_size = 0;
    const char *in_data = NULL;
    int has_input = !lua_isnoneornil(L, 2);

    if (has_input)
        in_data = luaL_checklstring(L, 2, &in_size);

    int in_pipe[2];
    int out_pipe[2];

    if (pipe(in_pipe) || pipe(out_pipe))
    {
        lua_pushnil(L);
        lua_pushinteger(L, -1);
        return 2;
    }

    pid_t pid = fork();
    if (pid == 0)
    {
        // child
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);

        close(in_pipe[1]);
        close(out_pipe[0]);

        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    // parent
    close(in_pipe[0]);
    close(out_pipe[1]);

    // write stdin
    if (has_input && in_size)
    {
        size_t off = 0;
        while (off < in_size)
        {
            ssize_t n = write(in_pipe[1], in_data + off, in_size - off);
            if (n <= 0)
                break;
            off += n;
        }
    }
    close(in_pipe[1]);

    luaL_Buffer out;
    luaL_buffinit(L, &out);

    unsigned char buf[4096];
    ssize_t n;
    while ((n = read(out_pipe[0], buf, sizeof(buf))) > 0)
        luaL_addlstring(&out, (const char *)buf, n);

    close(out_pipe[0]);

    int status;
    waitpid(pid, &status, 0);

    luaL_pushresult(&out);
    lua_pushinteger(L, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return 2;
}

#endif

static int is_directory(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

static char *path_join(const char *a, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char *r = malloc(la + lb + 2);
    memcpy(r, a, la);
    r[la] = PATH_SEPARATOR;
    memcpy(r + la + 1, b, lb + 1);
    return r;
}

static char *get_dirname(const char *path)
{
    char *dup = strdup(path);
#ifdef _WIN32
    char *p = strrchr(dup, '\\');
    if (!p)
        p = strrchr(dup, '/');
    if (p)
        *p = 0;
    else
        strcpy(dup, ".");
    return dup;
#else
    char *d = dirname(dup);
    char *r = strdup(d);
    free(dup);
    return r;
#endif
}

static char *get_basename(const char *path)
{
    char *dup = strdup(path);
#ifdef _WIN32
    char *p = strrchr(dup, '\\');
    if (!p)
        p = strrchr(dup, '/');
    return strdup(p ? p + 1 : dup);
#else
    char *b = basename(dup);
    char *r = strdup(b);
    free(dup);
    return r;
#endif
}

static char *get_filename_without_ext(const char *f)
{
    char *r = strdup(f);
    char *dot = strrchr(r, '.');
    if (dot)
        *dot = 0;
    return r;
}

// --------------------------------------------------
// Lua content building
// --------------------------------------------------

static void ensure_dir_table(lua_State *L, const char *rel)
{
    if (!rel || !*rel)
        return;
    lua_getglobal(L, "content");

    char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", rel);

    char *save = NULL;
    char *tok = strtok_r(buf, "/\\", &save);
    while (tok)
    {
        lua_getfield(L, -1, tok);
        if (lua_isnil(L, -1))
        {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_setfield(L, -2, tok);
            lua_getfield(L, -1, tok);
        }
        lua_remove(L, -2);
        tok = strtok_r(NULL, "/\\", &save);
    }
    lua_pop(L, 1);
}

static void add_file_to_table(lua_State *L, const char *rel, const char *full)
{
    FILE *fp = fopen(full, "rb");
    if (!fp)
        return;

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *data = malloc(len);
    fread(data, 1, len, fp);
    fclose(fp);

    char dir[PATH_MAX] = {0};
    char file[PATH_MAX] = {0};

    const char *p = strrchr(rel, PATH_SEPARATOR);
    if (p)
    {
        snprintf(dir, p - rel + 1, "%s", rel);
        snprintf(file, sizeof(file), "%s", p + 1);
    }
    else
        snprintf(file, sizeof(file), "%s", rel);

    ensure_dir_table(L, dir);
    lua_getglobal(L, "content");

    if (*dir)
    {
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s", dir);
        char *save = NULL;
        char *tok = strtok_r(tmp, "/\\", &save);
        while (tok)
        {
            lua_getfield(L, -1, tok);
            lua_remove(L, -2);
            tok = strtok_r(NULL, "/\\", &save);
        }
    }

    lua_newtable(L);
    lua_pushlstring(L, data, len);
    lua_setfield(L, -2, "rawData");
    lua_setfield(L, -2, file);
    lua_pop(L, 1);
    free(data);
}

static void traverse_dir(lua_State *L, const char *dir, const char *base, const char *builder)
{
    DIR *d = opendir(dir);
    if (!d)
        return;

    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (!strcmp(e->d_name, builder))
            continue;

        char *full = path_join(dir, e->d_name);
        const char *rel = full + strlen(base);
        if (*rel == PATH_SEPARATOR)
            rel++;

        if (is_directory(full))
        {
            ensure_dir_table(L, rel);
            traverse_dir(L, full, base, builder);
        }
        else
            add_file_to_table(L, rel, full);

        free(full);
    }
    closedir(d);
}

static void build_content_table(lua_State *L, const char *base, const char *builder)
{
    lua_newtable(L);
    lua_setglobal(L, "content");
    traverse_dir(L, base, base, builder);
}

// --------------------------------------------------
// serialization (FIXED)
// --------------------------------------------------

static void serialize_value(lua_State *L, int idx, FILE *fp)
{
    int t = lua_type(L, idx);
    unsigned char tb;

    if (t == LUA_TNIL)
    {
        tb = 0;
        fwrite(&tb, 1, 1, fp);
        return;
    }
    if (t == LUA_TSTRING)
    {
        tb = 1;
        fwrite(&tb, 1, 1, fp);
        size_t len;
        const char *s = lua_tolstring(L, idx, &len);
        int ilen = (int)len;
        fwrite(&ilen, sizeof(int), 1, fp);
        fwrite(s, 1, len, fp);
        return;
    }
    if (t == LUA_TTABLE)
    {
        tb = 2;
        fwrite(&tb, 1, 1, fp);
        int abs = lua_absindex(L, idx);
        int count = 0;
        lua_pushnil(L);
        while (lua_next(L, abs))
        {
            lua_pop(L, 1);
            count++;
        }
        fwrite(&count, sizeof(int), 1, fp);
        lua_pushnil(L);
        while (lua_next(L, abs))
        {
            serialize_value(L, -2, fp);
            serialize_value(L, -1, fp);
            lua_pop(L, 1);
        }
        return;
    }
    fprintf(stderr, "Unsupported lua type\n");
    exit(1);
}

static void serialize_table(lua_State *L, int idx, FILE *fp)
{
    serialize_value(L, idx, fp);
}

static void deserialize_value(lua_State *L, FILE *fp)
{
    unsigned char tb;
    if (fread(&tb, 1, 1, fp) != 1)
    {
        lua_pushnil(L);
        return;
    }

    if (tb == 0)
    {
        lua_pushnil(L);
        return;
    }
    if (tb == 1)
    {
        int len;
        fread(&len, sizeof(int), 1, fp);
        char *buf = malloc(len + 1);
        fread(buf, 1, len, fp);
        buf[len] = 0;
        lua_pushlstring(L, buf, len);
        free(buf);
        return;
    }
    if (tb == 2)
    {
        int count;
        fread(&count, sizeof(int), 1, fp);
        lua_newtable(L);
        for (int i = 0; i < count; i++)
        {
            deserialize_value(L, fp);
            deserialize_value(L, fp);
            lua_settable(L, -3);
        }
        return;
    }
}

static void deserialize_table(lua_State *L, FILE *fp)
{
    deserialize_value(L, fp);
}

// --------------------------------------------------
// unpack
// --------------------------------------------------

static void create_directories(const char *path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++)
    {
        if (*p == PATH_SEPARATOR)
        {
            *p = 0;
            mkdir(tmp, 0755);
            *p = PATH_SEPARATOR;
        }
    }
    mkdir(tmp, 0755);
}

static int key_has_extension(const char *key)
{
    return key && strchr(key, '.') != NULL;
}

static void unpack_table(lua_State *L, int idx, const char *base, const char *prefix)
{
    lua_pushnil(L);
    while (lua_next(L, idx))
    {
        const char *key = lua_tostring(L, -2);

        char rel[PATH_MAX];
        if (*prefix)
            snprintf(rel, sizeof(rel), "%s%c%s", prefix, PATH_SEPARATOR, key);
        else
            snprintf(rel, sizeof(rel), "%s", key);

        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s%c%s", base, PATH_SEPARATOR, rel);

        if (lua_istable(L, -1))
        {
            lua_getfield(L, -1, "rawData");
            if (lua_isstring(L, -1))
            {
                size_t len;
                const char *d = lua_tolstring(L, -1, &len);

                char dir[PATH_MAX];
                snprintf(dir, sizeof(dir), "%s", full);
                char *p = strrchr(dir, PATH_SEPARATOR);
                if (p)
                {
                    *p = 0;
                    create_directories(dir);
                }

                FILE *fp = fopen(full, "wb");
                if (fp)
                {
                    fwrite(d, 1, len, fp);
                    fclose(fp);
                }
                lua_pop(L, 1);
            }
            else
            {
                lua_pop(L, 1);

                /* no rawData */
                if (key_has_extension(key))
                {
                    /* treat as file, not directory */
                    char dir[PATH_MAX];
                    snprintf(dir, sizeof(dir), "%s", full);
                    char *p = strrchr(dir, PATH_SEPARATOR);
                    if (p)
                    {
                        *p = 0;
                        create_directories(dir);
                    }
                    FILE *fp = fopen(full, "wb");
                    if (fp)
                        fclose(fp);
                }
                else
                {
                    /* real directory */
                    create_directories(full);
                    unpack_table(L, lua_absindex(L, -1), base, rel);
                }
            }
        }
        lua_pop(L, 1);
    }
}

static int lua_fstring(lua_State *L)
{
    const char *fmt = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    luaL_Buffer b;
    luaL_buffinit(L, &b);

    const char *p = fmt;

    while (*p)
    {
        if (*p == '{')
        {
            const char *start = ++p;
            while (*p && *p != '}')
                p++;

            if (*p == '}')
            {
                lua_pushlstring(L, start, p - start);
                lua_gettable(L, 2); // table[key]

                if (!lua_isnil(L, -1))
                {
                    size_t len;
                    const char *value = luaL_tolstring(L, -1, &len);
                    luaL_addlstring(&b, value, len);
                    lua_pop(L, 2); // tolstring + value
                }
                else
                {
                    lua_pop(L, 1);
                }
                p++; // skip '}'
            }
            else
            {
                luaL_addchar(&b, '{');
            }
        }
        else
        {
            luaL_addchar(&b, *p++);
        }
    }

    luaL_pushresult(&b);
    return 1;
}

// --------------------------------------------------
// main
// --------------------------------------------------

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <builder.lua | file.bundle>\n", argv[0]);
        return 1;
    }

    char *base = get_dirname(argv[1]);
    char *file = get_basename(argv[1]);
    char *ext = strrchr(file, '.');
    if (!ext)
        return 1;

    lua_State *L = luaL_newstate();
    luaL_openlibs(L);

    if (!strcmp(ext, ".lua"))
    {
        char *name = get_filename_without_ext(file);
        build_content_table(L, base, file);

        lua_register(L, "fstring", lua_fstring);
        lua_register(L, "exec", lua_exec);

        lua_pushstring(L, platform);
        lua_setglobal(L, "platform");

        lua_pushstring(L, base);
        lua_setglobal(L, "directory");

        lua_pushstring(L, directorySeparator);
        lua_setglobal(L, "directorySeparator");

        if (luaL_dofile(L, argv[1]) != LUA_OK)
        {
            const char *err = lua_tostring(L, -1);
            fprintf(stderr, "Lua error: %s\n", err);
            lua_pop(L, 1);
            lua_close(L);
            return 1;
        }

        lua_getglobal(L, "content");
        lua_getfield(L, -1, "builder.lua");
        if (!lua_isnil(L, -1))
        {
            fprintf(stderr, "Warning: 'builder.lua' is a reserved service name "
                            "and is used to store the Lua code of the builder itself.");
        }
        lua_pop(L, 2);

        /* store builder.lua source into content["builder.lua"].rawData */
        FILE *bfp = fopen(argv[1], "rb");
        if (bfp)
        {
            fseek(bfp, 0, SEEK_END);
            long blen = ftell(bfp);
            fseek(bfp, 0, SEEK_SET);
            char *bdata = malloc(blen);
            fread(bdata, 1, blen, bfp);
            fclose(bfp);

            lua_getglobal(L, "content");
            lua_newtable(L);
            lua_pushlstring(L, bdata, blen);
            lua_setfield(L, -2, "rawData");
            lua_setfield(L, -2, "builder.lua");
            lua_pop(L, 1);

            free(bdata);
        }

        char bundle[PATH_MAX];
        snprintf(bundle, sizeof(bundle), "%s%c%s.bundle", base, PATH_SEPARATOR, name);
        FILE *fp = fopen(bundle, "wb");
        lua_getglobal(L, "content");
        serialize_table(L, lua_absindex(L, -1), fp);
        fclose(fp);
        lua_pop(L, 1);
        free(name);
    }
    else if (!strcmp(ext, ".bundle"))
    {
        FILE *fp = fopen(argv[1], "rb");
        deserialize_table(L, fp);
        fclose(fp);

        char *name = get_filename_without_ext(file);
        char out[PATH_MAX];
        snprintf(out, sizeof(out), "%s%c%s", base, PATH_SEPARATOR, name);
        create_directories(out);
        unpack_table(L, lua_absindex(L, -1), out, "");
        lua_pop(L, 1);
        free(name);
    }

    lua_close(L);
    free(base);
    free(file);
    return 0;
}
