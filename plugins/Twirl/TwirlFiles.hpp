/*
 * Twirl's own files, shared by every instance: its scenes, My toys, how it
 * looks (the page's level, numbers…). In a browser these live in the page's
 * storage; in the plugin, the web view's storage doesn't last (Qt 6's default
 * profile is off the record) and isn't shared, so they live here instead, in
 * $XDG_DATA_HOME/twirl (~/.local/share/twirl): scenes/Scene 1.json…, presets/<name>.json (named scenes), My
 * toys.json, settings/… .json. Plain JSON, the page's own; a scene file pastes
 * into Twirl's Paste a code (in the plugin or a browser).
 *
 * The page gets them all before it starts (initialJS: __twirlFiles, path →
 * base64), saves with setState("file", "path\ncontent"), and hears of
 * changes other instances made (twirlFiles({path: base64 or null})), looked
 * for once a second. All on the page's thread but initialJS() (before it).
 */

#ifndef TWIRL_FILES_HPP_INCLUDED
#define TWIRL_FILES_HPP_INCLUDED

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace twirl {

class Files
{
public:
    Files()
    {
        const char* const xdg = std::getenv("XDG_DATA_HOME");
        const char* const home = std::getenv("HOME");
        if (xdg != nullptr && *xdg == '/')
            fDir = std::string(xdg) + "/twirl";
        else if (home != nullptr)
            fDir = std::string(home) + "/.local/share/twirl";
    }

    /** For the page's initial JavaScript: every file, and where they are. */
    std::string initialJS()
    {
        std::string js = "window.__twirlFiles={";
        bool first = true;
        const Stamps now = scan();
        for (Stamps::const_iterator it = now.begin(); it != now.end(); ++it)
        {
            std::string content;
            if (! read(it->first, content))
                continue;
            fSeen[it->first] = it->second;
            js += (first ? "\"" : ",\"") + it->first + "\":\"" + base64(content) + "\"";
            first = false;
        }
        js += "};";
        if (safe(fDir, true))
            js += "window.__twirlFilesDir=\"" + fDir + "\";";
        return js;
    }

    /** From the page: "path\ncontent". Written whole (a temporary file, renamed); empty content deletes it. */
    void write(const char* const msg)
    {
        const char* const nl = std::strchr(msg, '\n');
        if (nl == nullptr || fDir.empty())
            return;
        const std::string path(msg, nl - msg);
        if (! safe(path, false))
            return;
        const std::string full = fDir + "/" + path;
        if (nl[1] == '\0')                     // sent empty: the page deleted it (a named scene)
        {
            std::remove(full.c_str());
            fSeen.erase(path);
            return;
        }
        makeDirs(full.substr(0, full.rfind('/')));
        const std::string tmp = full + ".tmp";
        std::FILE* const f = std::fopen(tmp.c_str(), "wb");
        if (f == nullptr)
            return;
        const size_t len = std::strlen(nl + 1);
        const bool ok = std::fwrite(nl + 1, 1, len, f) == len;
        if (std::fclose(f) != 0 || ! ok || std::rename(tmp.c_str(), full.c_str()) != 0)
        {
            std::remove(tmp.c_str());
            return;
        }
        fSeen[path] = stamp(full);
    }

    /** What changed since last asked (other instances' saves): JS for the page, or "". */
    std::string changes()
    {
        const Stamps now = scan();
        std::string js;
        for (Stamps::const_iterator it = now.begin(); it != now.end(); ++it)
        {
            const Stamps::const_iterator seen = fSeen.find(it->first);
            if (seen != fSeen.end() && seen->second == it->second)
                continue;
            std::string content;
            if (read(it->first, content))
            {
                js += (js.empty() ? "window.twirlFiles && twirlFiles({\"" : ",\"") + it->first + "\":\"" + base64(content) + "\"";
                fSeen[it->first] = it->second;
            }
        }
        for (Stamps::iterator it = fSeen.begin(); it != fSeen.end();)
        {
            if (now.count(it->first) == 0)
            {
                js += (js.empty() ? "window.twirlFiles && twirlFiles({\"" : ",\"") + it->first + "\":null";
                fSeen.erase(it++);
            }
            else
                ++it;
        }
        return js.empty() ? js : js + "})";
    }

private:
    typedef std::map<std::string, std::string> Stamps;   // path → modification time and size
    std::string fDir;
    Stamps fSeen;

    /** A path the page may use (relative, plain characters, .json), or the folder itself. */
    static bool safe(const std::string& p, const bool absolute)
    {
        if (p.empty() || p.size() > 400 || p.find("..") != std::string::npos)
            return false;
        for (size_t i = 0; i < p.size(); ++i)
        {
            const char c = p[i];
            if (! (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-' || c == '.' || c == '/'))
                return false;
        }
        if (absolute)
            return p[0] == '/';
        return p[0] != '/' && p.size() > 5 && p.compare(p.size() - 5, 5, ".json") == 0;
    }

    static std::string stamp(const std::string& full)
    {
        struct stat st;
        if (::stat(full.c_str(), &st) != 0)
            return std::string();
        char s[64];
        std::snprintf(s, sizeof(s), "%lld.%09ld/%lld", static_cast<long long>(st.st_mtim.tv_sec), st.st_mtim.tv_nsec,
                      static_cast<long long>(st.st_size));
        return s;
    }

    static void makeDirs(const std::string& dir)
    {
        for (size_t i = 1; i <= dir.size(); ++i)
            if (i == dir.size() || dir[i] == '/')
                ::mkdir(dir.substr(0, i).c_str(), 0755);
    }

    /** Every .json file under the folder (two levels: the folder and its folders). */
    Stamps scan() const
    {
        Stamps out;
        if (! fDir.empty())
            scanDir("", out, 0);
        return out;
    }

    void scanDir(const std::string& rel, Stamps& out, const int depth) const
    {
        DIR* const d = ::opendir((fDir + (rel.empty() ? "" : "/" + rel)).c_str());
        if (d == nullptr)
            return;
        while (const dirent* const e = ::readdir(d))
        {
            const std::string name = e->d_name;
            if (name[0] == '.')
                continue;
            const std::string path = rel.empty() ? name : rel + "/" + name;
            struct stat st;
            if (::stat((fDir + "/" + path).c_str(), &st) != 0)
                continue;
            if (S_ISDIR(st.st_mode) && depth < 1)
                scanDir(path, out, depth + 1);
            else if (S_ISREG(st.st_mode) && safe(path, false))
                out[path] = stamp(fDir + "/" + path);
        }
        ::closedir(d);
    }

    bool read(const std::string& path, std::string& content) const
    {
        std::FILE* const f = std::fopen((fDir + "/" + path).c_str(), "rb");
        if (f == nullptr)
            return false;
        content.clear();
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && content.size() < 512 * 1024)
            content.append(buf, n);
        std::fclose(f);
        return content.size() < 512 * 1024;
    }

    static std::string base64(const std::string& in)
    {
        static const char* const k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((in.size() + 2) / 3 * 4);
        size_t i = 0;
        for (; i + 2 < in.size(); i += 3)
        {
            const uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
            out += k[v >> 18]; out += k[(v >> 12) & 63]; out += k[(v >> 6) & 63]; out += k[v & 63];
        }
        if (i < in.size())
        {
            const uint32_t v = (uint8_t(in[i]) << 16) | (i + 1 < in.size() ? uint8_t(in[i + 1]) << 8 : 0);
            out += k[v >> 18]; out += k[(v >> 12) & 63];
            out += i + 1 < in.size() ? k[(v >> 6) & 63] : '=';
            out += '=';
        }
        return out;
    }
};

}

#endif // TWIRL_FILES_HPP_INCLUDED
