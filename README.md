# Student Query Management System (SQMS)

A client-server system written in C for a Computer Networks course project.
The web server is hand-written on raw POSIX TCP sockets: no HTTP library.

> Work in progress. Done so far: multi-threaded TCP server (step 1), HTTP/1.1 with web pages (step 2),
> login and cookie sessions with SQLite (step 3).

## Running it online: GitHub Codespaces (easiest)

1. Put this folder in a GitHub repository (on github.com: **New repository**, then **Add file → Upload files**,
   and drag in everything, including the hidden `.devcontainer` folder).
2. On the repository page: **Code → Codespaces → Create codespace on main**.
   The first start takes a few minutes while it installs the libraries listed in `.devcontainer/devcontainer.json`.
3. In the terminal at the bottom:
   ```bash
   make
   ./sqms_server --add-user teacher teacher1     # type a password (6+ chars) twice
   ./sqms_server
   ```
4. When the popup says port 8080 is available, click **Open in Browser** (or use the **Ports** tab).

> Uploading through the website can skip hidden folders such as `.devcontainer`. If libraries are missing,
> install them by hand: `sudo apt-get update && sudo apt-get install -y libsqlite3-dev`

## Running it on Windows (WSL) or Linux

```powershell
wsl --install        # Windows only: in an Administrator PowerShell, then restart
```
```bash
sudo apt update
sudo apt install -y build-essential libsqlite3-dev
cd /mnt/d/proj       # Windows drive D: inside WSL; on Linux, go to the project folder
make
./sqms_server
```
Then open **http://localhost:8080**.

Run the server from the project folder: it loads `templates/` and `static/` from the current directory
and creates `sqms.db` and `server.log` there. Stop it with **Ctrl+C**.

## Accounts

- **Students** register themselves at `/register`.
- **Teachers** are created on the server (so students cannot make themselves teachers):
  ```bash
  ./sqms_server --add-user teacher <username>
  ```
  The password is read without echo. For scripts it can be piped in:
  `echo 'secret123' | ./sqms_server --add-user student alice`
- Usernames: 3-32 characters of `a-z`, `0-9`, `_`. Passwords: 6-128 characters.
- To start over, stop the server and delete `sqms.db`.

## Pages

| URL         | Access   | What it shows |
|-------------|----------|---------------|
| `/`         | anyone   | Home page: roles, query states, links to the demos |
| `/echo`     | anyone   | Send a message with a GET form and a POST form; see how each travels over HTTP |
| `/inspect`  | anyone   | The exact request line and headers your browser sent (including the `Cookie` header), plus your IP:port |
| `/status`   | anyone   | Live connection counters; auto-refreshes every 5s via `<meta http-equiv="refresh">` |
| `/login`    | anyone   | Log in; sets the session cookie |
| `/register` | anyone   | Create a student account |
| `/logout`   | POST     | Ends the session and deletes the cookie |
| `/student`  | students | Student dashboard and session details |
| `/teacher`  | teachers | Teacher dashboard |

### Things to try in a demo
- Log in as a student, then type `/teacher` in the address bar: **403 Forbidden**.
- Log out, then open `/student`: **302** redirect to the login page.
- Enter a wrong password: **401 Unauthorized** with the same message for unknown users and wrong passwords.
- Open `/inspect` while logged in to see `Cookie: sqms_session=...` in the request headers.
- To demonstrate expiry, set `SESSION_TTL_SEC` to `60` in `include/config.h`, run `make`, log in and wait a minute.

## Database

SQLite file `sqms.db`. All queries use prepared statements.

| Table      | Columns |
|------------|---------|
| `users`    | `id`, `username` (unique), `pass_hash` (`pbkdf2_sha256$iterations$salt$hash`), `role` (`student`/`teacher`), `created_at` |
| `sessions` | `token` (64 hex chars), `user_id`, `expires_at` |

Look inside with: `sqlite3 sqms.db "SELECT id, username, role FROM users;"`

## Project layout

```
include/      header files
src/
  main.c        entry point, signal handling, --add-user
  server.c      sockets, accept loop, worker threads
  http.c        HTTP/1.1 request parser, cookies, response builder
  router.c      URL -> page handler, role-based access control
  auth.c        login, registration, sessions, cookie headers
  crypto.c      SHA-256, HMAC, PBKDF2 password hashing, /dev/urandom
  db.c          SQLite storage (prepared statements, mutex)
  template.c    {{var}} HTML templates (auto-escaped)
  strbuf.c      growable string buffer (overflow-safe)
  log.c         thread-safe logging
templates/    HTML pages
static/       CSS
.devcontainer/  GitHub Codespaces setup
Makefile
```
