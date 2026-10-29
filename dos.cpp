// RetroDOS — мини-эмулятор оболочки в стиле MS-DOS 80-х.
// Чистый C++17, без внешних зависимостей: только stdin/stdout + ANSI-цвета.
// Работает в Linux, в Termux на Android и как нативный бинарник через Android NDK.
//
// Сборка (Linux):  g++ -std=c++17 -O2 -o dos dos.cpp
// Запуск:          ./dos          (или ./dos --fast — без задержек при загрузке)

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std;

// ---------------------------------------------------------------- утилиты ---

static string upper(string s) {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

static string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static vector<string> tokens(const string& s) {
    vector<string> v;
    istringstream ss(s);
    string t;
    while (ss >> t) v.push_back(t);
    return v;
}

// ------------------------------------------------------- виртуальный диск ---

struct Node {
    string name;
    bool isDir = false;
    string data;
    Node* parent = nullptr;
    map<string, unique_ptr<Node>> kids;
};

class RetroDos {
public:
    explicit RetroDos(bool fast) : fast_(fast) {
        root_.isDir = true;
        cwd_ = &root_;
        env_["PATH"] = "C:\\";
        env_["COMSPEC"] = "C:\\COMMAND.COM";
        seed();
    }

    void boot();
    void run();

private:
    Node root_;
    Node* cwd_;
    map<string, string> env_;
    bool running_ = true;
    bool fast_;

    static constexpr size_t kDiskSize = 1457664;  // дискета 1.44 МБ

    // ---- файловая система ----

    Node* mk(Node* dir, const string& name, bool isDir, const string& data = "") {
        auto n = make_unique<Node>();
        n->name = name;
        n->isDir = isDir;
        n->data = data;
        n->parent = dir;
        Node* p = n.get();
        dir->kids[name] = move(n);
        return p;
    }

    void seed() {
        mk(&root_, "AUTOEXEC.BAT", false, "@ECHO OFF\nPATH C:\\\n");
        mk(&root_, "README.TXT", false,
           "Добро пожаловать в RetroDOS!\nНаберите HELP, чтобы увидеть список команд.\n");
        Node* games = mk(&root_, "GAMES", true);
        mk(games, "SNAKE.TXT", false, "Тут когда-нибудь будет змейка.\n");
        mk(&root_, "DOCS", true);
    }

    string pathOf(const Node* n) const {
        if (n == &root_) return "C:\\";
        vector<string> parts;
        for (const Node* c = n; c != &root_; c = c->parent) parts.push_back(c->name);
        string s = "C:";
        for (auto it = parts.rbegin(); it != parts.rend(); ++it) s += "\\" + *it;
        return s;
    }

    static string normalize(const string& raw, bool& absolute) {
        string p = upper(raw);
        if (p.size() >= 2 && p[1] == ':') p = p.substr(2);
        replace(p.begin(), p.end(), '/', '\\');
        absolute = !p.empty() && p[0] == '\\';
        return p;
    }

    Node* resolve(const string& raw) {
        bool abs;
        string p = normalize(raw, abs);
        Node* cur = abs ? &root_ : cwd_;
        istringstream ss(p);
        string part;
        while (getline(ss, part, '\\')) {
            if (part.empty() || part == ".") continue;
            if (!cur->isDir) return nullptr;
            if (part == "..") {
                if (cur->parent) cur = cur->parent;
                continue;
            }
            auto it = cur->kids.find(part);
            if (it == cur->kids.end()) return nullptr;
            cur = it->second.get();
        }
        return cur;
    }

    bool splitPath(const string& raw, Node*& dir, string& leaf) {
        bool abs;
        string p = normalize(raw, abs);
        while (p.size() > 1 && p.back() == '\\') p.pop_back();
        size_t pos = p.find_last_of('\\');
        if (pos == string::npos) {
            leaf = p;
            dir = cwd_;
            return !leaf.empty();
        }
        leaf = p.substr(pos + 1);
        string dirPart = p.substr(0, pos);
        if (dirPart.empty()) dirPart = "\\";
        dir = resolve(dirPart);
        return dir && dir->isDir && !leaf.empty();
    }

    static bool badName(const string& n) {
        return n.empty() || n == "." || n == ".." ||
               n.find_first_of("*?<>|:\"") != string::npos;
    }

    size_t usedBytes(const Node* n) const {
        size_t t = n->data.size();
        for (auto& kv : n->kids) t += usedBytes(kv.second.get());
        return t;
    }

    bool writeFile(const string& path, const string& content, bool append, ostream& o) {
        Node* dir;
        string leaf;
        if (!splitPath(path, dir, leaf) || badName(leaf)) {
            o << "Invalid path or file name\n";
            return false;
        }
        auto it = dir->kids.find(leaf);
        if (it != dir->kids.end()) {
            if (it->second->isDir) {
                o << "Access denied - " << leaf << "\n";
                return false;
            }
            if (append) it->second->data += content;
            else it->second->data = content;
        } else {
            mk(dir, leaf, false, content);
        }
        return true;
    }

    // ---- команды ----

    void cmdHelp(ostream& o) {
        o << "Команды RetroDOS:\n"
             "  DIR [путь]        список файлов        TYPE файл       показать файл\n"
             "  CD [путь]         сменить каталог      COPY a b        копировать\n"
             "  MD имя            создать каталог      COPY CON файл   ввод с клавиатуры (^Z - конец)\n"
             "  RD имя            удалить каталог      DEL файл        удалить файл\n"
             "  REN a b           переименовать        TREE            дерево каталогов\n"
             "  ECHO текст        вывести текст        ECHO текст > f  запись в файл (>> дописать)\n"
             "  SET [A=B]         переменные среды     VER DATE TIME   информация\n"
             "  MEM VOL           память, том          CLS             очистить экран\n"
             "  EXIT              выход\n";
    }

    void cmdDir(const string& args, ostream& o) {
        Node* n = args.empty() ? cwd_ : resolve(args);
        if (!n) {
            o << "File not found\n";
            return;
        }
        o << " Volume in drive C is RETRODOS\n";
        if (!n->isDir) {
            o << " Directory of " << pathOf(n->parent) << "\n\n";
            o << left << setw(16) << n->name << right << setw(10) << n->data.size() << "\n";
            return;
        }
        o << " Directory of " << pathOf(n) << "\n\n";
        size_t files = 0, dirs = 0, bytes = 0;
        if (n != &root_) {
            o << left << setw(16) << "." << "<DIR>\n";
            o << left << setw(16) << ".." << "<DIR>\n";
            dirs += 2;
        }
        for (auto& kv : n->kids) {
            const Node* k = kv.second.get();
            if (k->isDir) {
                o << left << setw(16) << k->name << "<DIR>\n";
                dirs++;
            } else {
                o << left << setw(16) << k->name << right << setw(10) << k->data.size() << "\n";
                files++;
                bytes += k->data.size();
            }
        }
        size_t used = usedBytes(&root_);
        o << right << setw(9) << files << " file(s)" << setw(12) << bytes << " bytes\n";
        o << setw(9) << dirs << " dir(s) " << setw(12)
          << (kDiskSize > used ? kDiskSize - used : 0) << " bytes free\n";
    }

    void cmdCd(const string& args, ostream& o) {
        if (args.empty()) {
            o << pathOf(cwd_) << "\n";
            return;
        }
        Node* n = resolve(args);
        if (!n || !n->isDir) o << "Invalid directory\n";
        else cwd_ = n;
    }

    void cmdMd(const string& args, ostream& o) {
        if (args.empty()) { o << "Required parameter missing\n"; return; }
        Node* dir;
        string leaf;
        if (!splitPath(args, dir, leaf) || badName(leaf)) {
            o << "Unable to create directory\n";
            return;
        }
        if (dir->kids.count(leaf)) {
            o << "Directory already exists\n";
            return;
        }
        mk(dir, leaf, true);
    }

    void cmdRd(const string& args, ostream& o) {
        Node* n = args.empty() ? nullptr : resolve(args);
        if (!n || !n->isDir || n == &root_) { o << "Invalid path, not directory,\n"; return; }
        if (!n->kids.empty()) { o << "Directory not empty\n"; return; }
        if (n == cwd_) cwd_ = n->parent;
        n->parent->kids.erase(n->name);
    }

    void cmdDel(const string& args, ostream& o) {
        Node* n = args.empty() ? nullptr : resolve(args);
        if (!n || n->isDir) { o << "File not found\n"; return; }
        n->parent->kids.erase(n->name);
    }

    void cmdRen(const vector<string>& t, ostream& o) {
        if (t.size() < 2) { o << "Required parameter missing\n"; return; }
        Node* n = resolve(t[0]);
        if (!n || n == &root_) { o << "File not found\n"; return; }
        string newName = upper(t[1]);
        if (badName(newName) || newName.find('\\') != string::npos) {
            o << "Invalid file name\n";
            return;
        }
        Node* dir = n->parent;
        if (dir->kids.count(newName)) { o << "Duplicate file name\n"; return; }
        auto holder = move(dir->kids[n->name]);
        dir->kids.erase(n->name);
        holder->name = newName;
        dir->kids[newName] = move(holder);
    }

    void cmdType(const string& args, ostream& o) {
        Node* n = args.empty() ? nullptr : resolve(args);
        if (!n || n->isDir) { o << "File not found\n"; return; }
        o << n->data;
        if (!n->data.empty() && n->data.back() != '\n') o << "\n";
    }

    void cmdCopy(const vector<string>& t, ostream& o) {
        if (t.size() < 2) { o << "Required parameter missing\n"; return; }
        if (upper(t[0]) == "CON") {
            string buf, line;
            o << "(введите текст, завершите строкой ^Z)\n";
            cout << flush;
            while (true) {
                if (!getline(cin, line)) { cin.clear(); break; }
                string tl = trim(line);
                if (tl == "^Z" || tl == "^z") break;
                buf += line + "\n";
            }
            if (writeFile(t[1], buf, false, o)) o << "        1 file(s) copied\n";
            return;
        }
        Node* src = resolve(t[0]);
        if (!src || src->isDir) { o << "File not found - " << t[0] << "\n"; return; }
        Node* dst = resolve(t[1]);
        string target = (dst && dst->isDir) ? pathOf(dst) + "\\" + src->name : t[1];
        string copy = src->data;
        if (writeFile(target, copy, false, o)) o << "        1 file(s) copied\n";
    }

    void tree(const Node* n, const string& prefix, ostream& o) {
        vector<const Node*> v;
        for (auto& kv : n->kids) v.push_back(kv.second.get());
        for (size_t i = 0; i < v.size(); ++i) {
            bool last = (i + 1 == v.size());
            o << prefix << (last ? "\\---" : "+---") << v[i]->name << "\n";
            if (v[i]->isDir) tree(v[i], prefix + (last ? "    " : "|   "), o);
        }
    }

    void cmdSet(const string& args, ostream& o) {
        if (args.empty()) {
            for (auto& kv : env_) o << kv.first << "=" << kv.second << "\n";
            return;
        }
        size_t eq = args.find('=');
        if (eq == string::npos) {
            auto it = env_.find(upper(trim(args)));
            if (it == env_.end()) o << "Environment variable not defined\n";
            else o << it->first << "=" << it->second << "\n";
            return;
        }
        string k = upper(trim(args.substr(0, eq))), v = trim(args.substr(eq + 1));
        if (v.empty()) env_.erase(k);
        else env_[k] = v;
    }

    void cmdDate(ostream& o) {
        time_t t = time(nullptr);
        tm* lt = localtime(&t);
        static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        o << "Current date is " << days[lt->tm_wday] << " " << setfill('0') << setw(2)
          << lt->tm_mon + 1 << "-" << setw(2) << lt->tm_mday << "-" << lt->tm_year + 1900
          << setfill(' ') << "\n";
    }

    void cmdTime(ostream& o) {
        time_t t = time(nullptr);
        tm* lt = localtime(&t);
        o << "Current time is " << setfill('0') << setw(2) << lt->tm_hour << ":" << setw(2)
          << lt->tm_min << ":" << setw(2) << lt->tm_sec << setfill(' ') << "\n";
    }

    void cmdMem(ostream& o) {
        size_t used = min<size_t>(usedBytes(&root_), 655360);
        o << "\n  655360 bytes total conventional memory\n"
          << "  " << setw(6) << used << " bytes used by RAM-disk\n"
          << "  " << setw(6) << 655360 - used << " bytes free\n";
    }

    // ---- разбор и выполнение строки ----

    void exec(const string& rawLine, ostream& o) {
        string line = trim(rawLine);
        if (line.empty()) return;

        // "CD.." / "CD\" без пробела, как в настоящем DOS
        string head = upper(line.substr(0, min<size_t>(3, line.size())));
        string cmd, args;
        if (head.size() == 3 && head.substr(0, 2) == "CD" &&
            (head[2] == '.' || head[2] == '\\' || head[2] == '/')) {
            cmd = "CD";
            args = trim(line.substr(2));
        } else {
            size_t sp = line.find_first_of(" \t");
            cmd = upper(line.substr(0, sp));
            args = sp == string::npos ? "" : trim(line.substr(sp + 1));
        }
        vector<string> t = tokens(args);

        if (cmd == "HELP" || cmd == "?") cmdHelp(o);
        else if (cmd == "DIR") cmdDir(args, o);
        else if (cmd == "CD" || cmd == "CHDIR") cmdCd(args, o);
        else if (cmd == "MD" || cmd == "MKDIR") cmdMd(args, o);
        else if (cmd == "RD" || cmd == "RMDIR") cmdRd(args, o);
        else if (cmd == "DEL" || cmd == "ERASE") cmdDel(args, o);
        else if (cmd == "REN" || cmd == "RENAME") cmdRen(t, o);
        else if (cmd == "TYPE") cmdType(args, o);
        else if (cmd == "COPY") cmdCopy(t, o);
        else if (cmd == "TREE") { o << pathOf(cwd_) << "\n"; tree(cwd_, "", o); }
        else if (cmd == "ECHO") o << (args.empty() ? "ECHO is on" : args) << "\n";
        else if (cmd == "ECHO.") o << "\n";
        else if (cmd == "SET") cmdSet(args, o);
        else if (cmd == "VER") o << "\nRetroDOS [Version 1.00]\n";
        else if (cmd == "DATE") cmdDate(o);
        else if (cmd == "TIME") cmdTime(o);
        else if (cmd == "MEM") cmdMem(o);
        else if (cmd == "VOL") o << " Volume in drive C is RETRODOS\n Volume Serial Number is 1984-0C0D\n";
        else if (cmd == "CLS") o << "\033[2J\033[H";
        else if (cmd == "EXIT" || cmd == "QUIT") running_ = false;
        else if (cmd == "C:") cwd_ = &root_;
        else o << "Bad command or file name\n";
    }

    void execLine(const string& rawLine) {
        // перенаправление вывода: cmd > file  /  cmd >> file
        size_t gt = rawLine.find('>');
        if (gt == string::npos) {
            exec(rawLine, cout);
            return;
        }
        bool append = gt + 1 < rawLine.size() && rawLine[gt + 1] == '>';
        string left = rawLine.substr(0, gt);
        string target = trim(rawLine.substr(gt + (append ? 2 : 1)));
        if (target.empty()) {
            cout << "Syntax error\n";
            return;
        }
        ostringstream buf;
        exec(left, buf);
        writeFile(target, buf.str(), append, cout);
    }

    void pause(int ms) {
        if (!fast_) this_thread::sleep_for(chrono::milliseconds(ms));
    }
};

// ---------------------------------------------------------- загрузка и REPL

void RetroDos::boot() {
    cout << "\033[2J\033[H\033[32m";  // чистый экран, зелёный текст
    cout << "RetroBIOS v1.0  (C) 1984 Retro Systems Inc.\n\n";
    for (int k = 0; k <= 640; k += 64) {
        cout << "\rMemory Test: " << k << "K OK" << flush;
        pause(70);
    }
    cout << "\n\n";
    pause(300);
    cout << "Starting RetroDOS...\n";
    pause(500);
    cout << "\nRetroDOS [Version 1.00]\n(C) Copyright Retro Systems 1984.\n\n";

    // выполняем AUTOEXEC.BAT «для атмосферы»: только PATH
    cout << "Type HELP for a list of commands.\n\n";
}

void RetroDos::run() {
    string line;
    while (running_) {
        cout << pathOf(cwd_) << ">" << flush;
        if (!getline(cin, line)) break;  // Ctrl+D
        execLine(line);
    }
    cout << "\033[0m\n";  // вернуть цвета терминала
}

int main(int argc, char** argv) {
    bool fast = false;
    for (int i = 1; i < argc; ++i)
        if (string(argv[i]) == "--fast") fast = true;
    ios::sync_with_stdio(true);
    RetroDos dos(fast);
    dos.boot();
    dos.run();
    return 0;
}
