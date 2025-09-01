# 🐚 MiniShell – A Custom UNIX Shell in C  

**MiniShell** is a lightweight custom-built command-line shell written in **C** as part of an **Operating Systems project**.  
It replicates core features of a real shell, making command execution easier while demonstrating **process management, system calls, and I/O handling**.  

---

## ✨ Features
- ✅ Run **basic commands** (`ls`, `pwd`, `echo hello`)  
- ✅ **Background execution** (`&`) – run multiple processes at once  
- ✅ **Input/Output Redirection** (`<`, `>`, `>>`)  
- ✅ **Piping** (`|`) – chain multiple commands  
- ✅ **Job Management** (`jobs`, `fg`, `kill`)  
- ✅ **Signal Handling** (`Ctrl+C`, `Ctrl+Z`)  

---

## 🎯 Why This Project?
- Understand how **shells work internally**  
- Practice **fork(), execvp(), waitpid(), dup2()** system calls  
- Demonstrate **process scheduling & management** concepts from OS  

---

## 🛠️ Tech Stack
- **Language:** C  
- **OS Concepts:** Process Management, I/O Redirection, Inter-process Communication (IPC)  
- **Environment:** Linux/Unix  

---

## 🚀 Getting Started  

 1. Clone the repo
```bash
git clone https://github.com/<your-username>/MiniShell.git
cd MiniShell
2. Build
make
3. Run
./myshell
PROJECT STRUCTURE:-
MiniShell/
├── src/
│   ├── main.c        # Entry point for the shell
│   ├── parser.c      # Command parsing
│   ├── executor.c    # Execution & redirection handling
│   ├── jobs.c        # Background jobs & process management
│   └── signals.c     # Signal handling (Ctrl+C, Ctrl+Z)
├── include/          # Header files
├── Makefile          # Build script
└── README.md         # Project documentation

