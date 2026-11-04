# Student Query Management System (SQMS)

A client-server system in C for a Computer Networks course project. Students ask questions, an LLM
drafts an answer, a teacher approves or edits it, and the final answer goes back to the student.

The web server is written by hand on raw POSIX TCP sockets: no HTTP library. The HTTP parser, the
response builder, sessions, the template engine and password hashing are all in `src/`.

For the design, the message-flow diagram and the mapping from features to networking concepts,
see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## 1. Build and run

### GitHub Codespaces (easiest)
1. Put this folder in a GitHub repository, including the hidden `.devcontainer` folder.
2. On the repository page: **Code → Codespaces → Create codespace on main**.
3. In the terminal:
   ```bash
   make
   ./sqms_server --add-user teacher teacher1     # type a password (6+ chars) twice
   ./sqms_server
   ```
4. When the popup says port 8080 is available, click **Open in Browser** (or use the **Ports** tab).

If `make` reports a missing header (`sqlite3.h`, `curl/curl.h`, `cjson/cJSON.h`), install the libraries:
```bash
sudo apt-get update && sudo apt-get install -y libsqlite3-dev libcurl4-openssl-dev libcjson-dev libssl-dev
```

### Linux or WSL
```bash
sudo apt install -y build-essential libsqlite3-dev libcurl4-openssl-dev libcjson-dev
make
./sqms_server               # http://localhost:8080
```

Run the server from the project folder: it loads `templates/` and `static/` from the current
directory and creates `sqms.db` and `server.log` there. Stop it with **Ctrl+C**.

### Dependencies
| Package | Used for |
|---|---|
| `build-essential` (gcc, make) | compiling |
| `libsqlite3-dev` | storage (users, sessions, queries) |
| `libcurl4-openssl-dev` | HTTP client calls to the LLM API |
| `libcjson-dev` | building and parsing the LLM's JSON |
| `libssl-dev` | optional HTTPS bonus (`make HTTPS=1`) |

---

## 2. Accounts

- **Students** register themselves at `/register` (or with the CLI client).
- **Teachers** are created on the server, so a student cannot make themselves a teacher:
  ```bash
  ./sqms_server --add-user teacher <username>
  ```
  The password is read without echo. Scripts can pipe it in:
  `echo 'secret123' | ./sqms_server --add-user student alice`
- Usernames: 3-32 characters of `a-z`, `0-9`, `_`. Passwords: 6-128 characters.
- To start over, stop the server and delete `sqms.db`.

---

## 3. Choosing the LLM

Background threads send each new question to an LLM and store its draft. Configure it with
environment variables **before** starting the server. With nothing set the server runs in **MOCK**
mode: no network, a canned draft after about 3 seconds (it knows a few topics such as TCP, UDP,
HTTP, DNS and sockets).

**Any OpenAI-compatible API** (OpenAI, Groq, OpenRouter, Gemini, ...):
```bash
export LLM_PROVIDER=openai
export LLM_API_KEY=your-key-here
# Optional; defaults are OpenAI's endpoint and gpt-4o-mini. Example for Groq (free tier):
export LLM_ENDPOINT=https://api.groq.com/openai/v1/chat/completions
export LLM_MODEL=llama-3.1-8b-instant
./sqms_server
```
For Google Gemini: `LLM_ENDPOINT=https://generativelanguage.googleapis.com/v1beta/openai/chat/completions`
and `LLM_MODEL=gemini-2.0-flash`.

**Local Ollama** (no key; default `http://localhost:11434/api/chat`, model `llama3.2`):
```bash
ollama pull llama3.2
export LLM_PROVIDER=ollama
./sqms_server
```

Never commit an API key. In Codespaces, add it under **GitHub → Settings → Codespaces → Secrets**
as `LLM_API_KEY`, or `export` it in the terminal each time.

If the LLM cannot be reached after 3 tries, the question still moves on to the teacher with a note
asking them to write the answer by hand.

---

## 4. How to check that everything works

Run the server in one terminal (`./sqms_server`) and use a second terminal for commands
(the **+** button in the Codespaces terminal panel). Tick off each check.

### A. The website (steps 2-6)
| # | Do this | You should see |
|---|---|---|
| A1 | Open the home page | The SQMS home page with styling |
| A2 | Open `/echo`, send a message with **GET**, then with **POST** | GET: message in the address bar. POST: clean URL. Both are echoed back |
| A3 | On `/echo` send `<b>hi</b>` | Shown as plain text, not bold (HTML escaping) |
| A4 | Open `/inspect`, reload twice | Your request headers; the port number changes on every reload |
| A5 | Open `/nothing-here` | A 404 page |
| A6 | Register a student at `/register` | You land on **My questions** |
| A7 | Click **Ask a question**, ask *"What is the TCP three-way handshake?"* | The question page, step 1 highlighted |
| A8 | Wait ~3 seconds without touching anything | The page refreshes itself; step 2 highlighted, status *Being reviewed by a teacher*. The student does **not** see the AI draft |
| A9 | In a **private/incognito window**, log in as the teacher | The **Review queue** with the question in it |
| A10 | Click **Review**, change a word in the answer, click **Send answer to student** | Back on the queue with "Answer to question #N sent"; it appears under *Recently reviewed* as *Edited, not yet read* (or already *Delivered* if the student's page refreshed in the meantime) |
| A11 | Back in the student window (it refreshes on its own) | Step 3 then step 4: **Delivered**, with the teacher's answer |
| A12 | Teacher window, reload the queue | The question now shows *Delivered* |
| A13 | Ask another question, and as teacher send the draft **unchanged** | `server.log` says `LLM_ANSWERED -> TEACHER_APPROVED` instead of `TEACHER_EDITED`, and the student's answer has no "edited" note |

### B. Security and error codes
| # | Do this | Expected |
|---|---|---|
| B1 | As a student, open `/teacher` | **403 Forbidden** |
| B2 | As a student, open `/query?id=N` for another student's question | **403 Forbidden** |
| B3 | Open `/query?id=99999` / `/query?id=abc` | **404** / **400** |
| B4 | Log in with a wrong password | **401**, "Invalid username or password" |
| B5 | Log out, then open `/student` | Redirected (**302**) to the login page |
| B6 | Open `/static/../src/main.c` | **404**, source code is not served |
| B7 | `./sqms_client -v get /status` and look for `Set-Cookie` after a login | `HttpOnly; SameSite=Lax` on the cookie |

### C. Command-line client (step 7)
```bash
./sqms_client register bob secret123      # Registered and logged in as bob
./sqms_client ask "What is a socket?"     # Question #N submitted (state PENDING_LLM)
./sqms_client wait N                      # Question #N reached LLM_ANSWERED after ~3 s
./sqms_client list                        # #N  LLM_ANSWERED  What is a socket?
./sqms_client show N                      # details (answer appears after teacher review)
./sqms_client -v get /                    # raw HTTP: request headers (>) and response headers (<)
./sqms_client logout
```

### D. Many students at once (step 7)
```bash
bash tests/load_test.sh 20      # 20 simulated students, all at the same moment
```
Expected: `Succeeded: 20`, `Failed: 0`. With MOCK mode and 2 LLM workers, the last student waits about
20 × 3 s / 2 = 30 s, which shows the queue working. Watch `/status` in the browser while it runs.
Try `bash tests/load_test.sh 50` as well.

### E. Limits and robustness
| # | Do this | Expected |
|---|---|---|
| E1 | `bash tests/limits_test.sh connections` | The extra request gets **503**; after closing, **200** |
| E2 | `nc localhost 8080`, type nothing, wait 15 s | Server closes the connection (receive timeout); log says "idle" |
| E3 | `head -c 40000 /dev/zero \| curl -s -o /dev/null -w '%{http_code}\n' --data-binary @- localhost:8080/echo` | **413** (body too large) |
| E4 | `printf 'BROKEN\r\n\r\n' \| nc localhost 8080` | **400 Bad Request** |
| E5 | During the load test, press Ctrl+C in a client terminal | Server keeps running (SIGPIPE ignored) |
| E6 | Stop the server with Ctrl+C while a question is PENDING_LLM, start it again | Log says "re-queued 1 question(s)"; it gets its draft |

### F. Logs and database
```bash
tail -f server.log                                  # connections, requests, state changes
sqlite3 sqms.db "SELECT id, state, teacher_id FROM queries;"
sqlite3 sqms.db "SELECT id, username, role, substr(pass_hash,1,30) FROM users;"
```
In `server.log` one question's life looks like:
```
[conn 12] 127.0.0.1:51432 query #3 created by alice -> PENDING_LLM
[llm 1] query #3: PENDING_LLM -> LLM_ANSWERED (3.0 s)
[conn 20] 127.0.0.1:51460 query #3: LLM_ANSWERED -> TEACHER_EDITED by teacher1
[conn 25] 127.0.0.1:51488 query #3: TEACHER_EDITED -> DELIVERED (answer sent to alice)
```

### G. Bonus: HTTPS and rate limiting (step 8)
```bash
make clean && make HTTPS=1      # needs libssl-dev
make cert                       # self-signed certificate in certs/
./sqms_server --https           # https://localhost:8443
curl -k -v https://localhost:8443/status 2>&1 | grep -E 'SSL connection|HTTP/1.1'
```
Expected: `SSL connection using TLSv1.3` and `HTTP/1.1 200 OK`. `/inspect` shows "HTTPS - encrypted
with TLSv1.3". The session cookie now also has `Secure`. Browsers warn about a self-signed certificate;
that is expected (click *Advanced → Proceed*).

Rate limiting:
```bash
RATE_LIMIT=5 ./sqms_server                # 5 requests/s per IP, burst 10
bash tests/limits_test.sh ratelimit       # in the other terminal: some HTTP 429 responses
```
Go back to a normal `make clean && make` afterwards if you want the plain build.

---

## 5. Capturing traffic with tcpdump and Wireshark

The server uses one TCP connection per request (`Connection: close`), so every request shows its
own complete handshake, request, response and close.

### Capture
```bash
# Live, printing packet contents as text (loopback interface, port 8080):
sudo tcpdump -i lo -nn -A 'tcp port 8080'

# Or save to a file for Wireshark:
sudo tcpdump -i lo -nn -w capture.pcap 'tcp port 8080'
# ... in another terminal: ./sqms_client get /status   (or use the browser), then Ctrl+C
```
In Codespaces, right-click `capture.pcap` in the file explorer → **Download**, then open it in
Wireshark on your computer. On your own Linux/WSL machine you can run Wireshark directly and capture
on the **Loopback (lo)** interface.

### What to look for in Wireshark
| Filter | Shows |
|---|---|
| `tcp.port == 8080` | everything for the server |
| `tcp.flags.syn == 1` | the SYN and SYN-ACK of each three-way handshake |
| `http` | only HTTP requests and responses |
| `http.request.method == "POST"` | form submissions (login, ask, review) |
| `http.cookie` / `http.set_cookie` | the session cookie being set and sent back |
| `tcp.flags.fin == 1` | connection closes |

For one request you will see, in order:
1. **SYN** (client → server), **SYN, ACK** (server → client), **ACK**: the three-way handshake.
2. **HTTP request**, e.g. `POST /login HTTP/1.1` with the form in the body.
3. **HTTP response**, e.g. `HTTP/1.1 302 Found` with `Set-Cookie` and `Location`.
4. **FIN, ACK** from the server (it closes first), then FIN/ACK from the client: the four-way close.

Right-click any packet → **Follow → TCP Stream** to read the whole conversation as text.

Things worth showing in the viva:
- Over HTTP, the password in `POST /login` is visible in plain text. Capture the same login with
  `--https` on port 8443 (`tcp port 8443`): Wireshark only shows **TLS Client Hello / Server Hello**
  and encrypted *Application Data*. That is why HTTPS matters.
- On a student's question page, a new TCP connection appears every 5 seconds: meta-refresh polling.
- During `load_test.sh`, many handshakes interleave: concurrent clients, each on its own port.

---

## 6. Pages

| URL | Access | What it shows |
|---|---|---|
| `/` | anyone | Home page |
| `/echo` | anyone | GET vs POST form demo |
| `/inspect` | anyone | The request your browser sent, your IP:port, HTTP or HTTPS |
| `/status` | anyone | Live counters: connections, questions by state, LLM queue; refreshes every 5 s |
| `/login`, `/register` | anyone | Log in / create a student account |
| `/logout` | POST | End the session |
| `/student` | students | My questions; polls every 10 s while any is open |
| `/ask` | students | Ask a new question |
| `/query?id=N` | owner or teachers | One question with its progress; polls every 5 s until answered |
| `/teacher` | teachers | Review queue and recently reviewed questions; polls every 10 s |
| `/review?id=N` | teachers | Approve or edit one AI draft |
| `/api/queries`, `/api/query?id=N` | as above | Plain-text versions for the CLI client |

---

## 7. Configuration reference

Compile-time limits are in [include/config.h](include/config.h). The main ones:

| Setting | Default | Meaning |
|---|---|---|
| `DEFAULT_PORT` | 8080 | HTTP port (`./sqms_server 9000` overrides) |
| `MAX_CONNECTIONS` | 64 | simultaneous connections; extra get 503 |
| `SOCKET_TIMEOUT_SEC` | 15 | recv/send timeout per connection |
| `MAX_HEADER_SIZE` / `MAX_BODY_SIZE` | 8 KB / 32 KB | request size limits (431 / 413) |
| `SESSION_TTL_SEC` | 3600 | session and cookie lifetime |
| `LLM_WORKERS` | 2 | background LLM threads |
| `PBKDF2_ITERATIONS` | 100000 | password hashing cost |

Environment variables: `LLM_PROVIDER`, `LLM_ENDPOINT`, `LLM_MODEL`, `LLM_API_KEY`, `RATE_LIMIT`.

---

## 8. Project layout

```
include/            header files
src/
  main.c              entry point, signals, --add-user, --https
  server.c            sockets, accept loop, worker threads, connection limits
  http.c              HTTP/1.1 parser, cookies, response builder
  router.c            URL -> handler, role-based access control, all pages
  auth.c              registration, login, sessions, cookie headers
  crypto.c            SHA-256, HMAC, PBKDF2 password hashing, /dev/urandom
  db.c                SQLite storage (prepared statements, mutex)
  llm.c               LLM client (libcurl + cJSON), job queue, worker threads, MOCK mode
  tls.c               optional HTTPS with OpenSSL
  ratelimit.c         optional per-IP token-bucket rate limiting
  template.c          {{var}} HTML templates (auto-escaped)
  strbuf.c            growable string buffer (overflow-safe)
  util.c, log.c       file reading, thread-safe logging
client/sqms_client.c  command-line client on raw sockets
tests/                load_test.sh (many students), limits_test.sh (503 / 429)
templates/            HTML pages
static/               CSS
docs/ARCHITECTURE.md  design and report material
.devcontainer/        GitHub Codespaces setup
Makefile
```

### Database tables
| Table | Columns |
|---|---|
| `users` | `id`, `username` (unique), `pass_hash` (`pbkdf2_sha256$iterations$salt$hash`), `role`, `created_at` |
| `sessions` | `token` (64 hex chars), `user_id`, `expires_at` |
| `queries` | `id`, `student_id`, `question`, `state`, `llm_answer`, `final_answer`, `teacher_id`, `created_at`, `updated_at` |
