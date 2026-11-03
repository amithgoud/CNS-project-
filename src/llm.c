/*
 * llm.c - LLM client (libcurl + cJSON) and the worker pool that drafts
 * answers in the background. See llm.h for configuration.
 *
 * ---------------------------------------------------------------------
 * Concurrency: producer/consumer queue
 * ---------------------------------------------------------------------
 * Producers are the web worker threads (POST /ask); consumers are the
 * LLM_WORKERS threads below. The queue is a fixed-size ring buffer
 * protected by job_lock; consumers sleep on the job_ready condition
 * variable while it is empty, so idle workers use no CPU.
 *
 * Why not call the LLM directly inside the POST /ask request? An LLM call
 * takes seconds. Doing it in the background lets the server answer the
 * student at once (302 -> the question page), keeps the web thread free,
 * and caps the number of simultaneous LLM calls at LLM_WORKERS no matter
 * how many students ask at the same moment.
 */
#include "llm.h"
#include "config.h"
#include "db.h"
#include "log.h"
#include "strbuf.h"

#include <cjson/cJSON.h>
#include <ctype.h>
#include <curl/curl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum { PROVIDER_MOCK, PROVIDER_OPENAI, PROVIDER_OLLAMA } provider_t;

static provider_t provider = PROVIDER_MOCK;
static char endpoint[512];
static char model[128];
static char api_key[512];

static const char SYSTEM_PROMPT[] =
    "You are a teaching assistant for a university Computer Networks course. "
    "Answer the student's question clearly and correctly in at most 250 words. "
    "Use plain text, no Markdown. A teacher will review your answer before "
    "the student sees it, so say so if you are unsure.";

/* ---- job queue (ring buffer), protected by job_lock ---------------- */
static long long       jobs[LLM_QUEUE_CAP];
static int             job_head = 0;
static int             job_count = 0;
static int             busy_workers = 0;
static pthread_mutex_t job_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  job_ready = PTHREAD_COND_INITIALIZER;

/* Read by workers and by libcurl's progress callback without the lock. */
static atomic_int stopping = 0;

static pthread_t workers[LLM_WORKERS];
static int       worker_count = 0;

static const char *provider_name(provider_t p)
{
    return p == PROVIDER_OPENAI ? "openai" : p == PROVIDER_OLLAMA ? "ollama" : "mock";
}

/* ------------------------------------------------------------------ */
/* Queue                                                              */
/* ------------------------------------------------------------------ */

int llm_enqueue(long long query_id)
{
    pthread_mutex_lock(&job_lock);
    if (job_count == LLM_QUEUE_CAP) {
        pthread_mutex_unlock(&job_lock);
        log_warn("llm: queue full, query #%lld stays PENDING_LLM", query_id);
        return -1;
    }
    jobs[(job_head + job_count) % LLM_QUEUE_CAP] = query_id;
    job_count++;
    pthread_cond_signal(&job_ready);        /* wake one sleeping worker */
    pthread_mutex_unlock(&job_lock);
    return 0;
}

/* Block until a job is available. Returns its id, or -1 when stopping. */
static long long dequeue(void)
{
    long long id = -1;

    pthread_mutex_lock(&job_lock);
    /* while, not if: wake-ups can be spurious, and another worker may
     * have taken the job first. */
    while (job_count == 0 && !atomic_load(&stopping))
        pthread_cond_wait(&job_ready, &job_lock);
    if (!atomic_load(&stopping)) {
        id = jobs[job_head];
        job_head = (job_head + 1) % LLM_QUEUE_CAP;
        job_count--;
        busy_workers++;
    }
    pthread_mutex_unlock(&job_lock);
    return id;
}

static void job_done(void)
{
    pthread_mutex_lock(&job_lock);
    busy_workers--;
    pthread_mutex_unlock(&job_lock);
}

void llm_get_status(llm_status_t *out)
{
    out->provider = provider_name(provider);
    out->model    = model;
    out->endpoint = endpoint;
    out->workers  = worker_count;
    pthread_mutex_lock(&job_lock);
    out->queued = job_count;
    out->busy   = busy_workers;
    pthread_mutex_unlock(&job_lock);
}

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Sleep in 100 ms steps so shutdown is never delayed by a backoff. */
static void sleep_ms(long ms)
{
    while (ms > 0 && !atomic_load(&stopping)) {
        long step = ms < 100 ? ms : 100;
        struct timespec ts = { 0, step * 1000000L };
        nanosleep(&ts, NULL);
        ms -= step;
    }
}

/* Trim whitespace in place and cap the length on a UTF-8 boundary.
 * Returns a pointer into s. */
static char *tidy_answer(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    if (n > LLM_MAX_ANSWER) {
        n = LLM_MAX_ANSWER;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
            n--;
        s[n] = '\0';
    }
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
    return s;
}

/* ------------------------------------------------------------------ */
/* MOCK mode                                                          */
/* ------------------------------------------------------------------ */

/* A few canned networking answers so offline demos still look sensible.
 * More specific keywords come first ("handshake" before "tcp"). */
static const struct {
    const char *keyword;
    const char *text;
} MOCK_TOPICS[] = {
    { "handshake",
      "TCP opens a connection with a three-way handshake. The client sends a SYN "
      "segment with its initial sequence number; the server replies with SYN-ACK, "
      "acknowledging it and sending its own sequence number; the client answers with "
      "ACK. After that both sides know the other is reachable and have agreed on "
      "sequence numbers, so lost or reordered data can be detected." },
    { "udp",
      "UDP is a connectionless transport protocol. It sends independent datagrams with "
      "no handshake, no acknowledgements and no retransmission, so it is fast and has "
      "low overhead but delivery and order are not guaranteed. It suits DNS lookups, "
      "streaming and games, where speed matters more than perfect reliability." },
    { "tcp",
      "TCP is a connection-oriented, reliable byte-stream protocol. It numbers every "
      "byte, acknowledges what arrives, retransmits lost segments and delivers data in "
      "order. Flow control (the receive window) stops a fast sender overwhelming the "
      "receiver, and congestion control (slow start, congestion avoidance) protects "
      "the network." },
    { "http",
      "HTTP is a request/response application protocol that runs over TCP. The client "
      "sends a request line (method, path, version), headers and an optional body; the "
      "server replies with a status line such as 200 OK, headers like Content-Type and "
      "Content-Length, and a body. HTTP is stateless, so sites use cookies to keep a "
      "user logged in across requests." },
    { "dns",
      "DNS translates names like example.com into IP addresses. A resolver asks a root "
      "server, then the top-level-domain server (.com), then the domain's authoritative "
      "server, caching each answer for its TTL. Queries normally use UDP port 53." },
    { "subnet",
      "Subnetting splits an IP network into smaller networks by borrowing host bits for "
      "the network part. For example 192.168.1.0/24 can be split into four /26 subnets "
      "of 64 addresses each (62 usable hosts, minus network and broadcast addresses)." },
    { "socket",
      "A socket is an endpoint for network communication, identified by an IP address "
      "and port. A TCP server calls socket(), bind(), listen() and accept(); a client "
      "calls socket() and connect(). After that both sides use send() and recv() on the "
      "connected socket, and close() when done." },
    { "osi",
      "The OSI model has seven layers: physical, data link, network, transport, session, "
      "presentation and application. Each layer serves the one above it; for example IP "
      "works at the network layer, TCP and UDP at the transport layer, and HTTP at the "
      "application layer." },
};

static char *mock_answer(const char *question)
{
    char *lower = strdup(question);
    if (!lower)
        return NULL;
    for (char *p = lower; *p; p++)
        *p = (char)tolower((unsigned char)*p);

    const char *topic = NULL;
    for (size_t i = 0; i < sizeof MOCK_TOPICS / sizeof MOCK_TOPICS[0]; i++) {
        if (strstr(lower, MOCK_TOPICS[i].keyword)) {
            topic = MOCK_TOPICS[i].text;
            break;
        }
    }
    free(lower);

    sleep_ms(LLM_MOCK_DELAY_MS);        /* pretend to think */

    strbuf_t sb;
    sb_init(&sb);
    sb_puts(&sb, "[MOCK DRAFT - the server is in offline MOCK mode, no real AI was contacted]\n\n");
    sb_puts(&sb, topic ? topic
                       : "This is a placeholder draft. Set LLM_PROVIDER to openai or ollama "
                         "to get a real answer from a language model. The teacher should "
                         "replace this text with the actual answer before sending it.");
    if (sb.oom) {
        sb_free(&sb);
        return NULL;
    }
    return sb.data;                     /* caller frees */
}

/* ------------------------------------------------------------------ */
/* Real LLM over HTTP (libcurl + cJSON)                               */
/* ------------------------------------------------------------------ */

/* libcurl calls this for each chunk of the response body. */
static size_t on_body(char *data, size_t size, size_t nmemb, void *userdata)
{
    strbuf_t *sb = userdata;
    size_t n = size * nmemb;
    if (sb->len + n > LLM_MAX_RESPONSE || sb_append(sb, data, n) < 0)
        return 0;                       /* returning less aborts the transfer */
    return n;
}

/* libcurl calls this periodically; non-zero aborts (used on shutdown). */
static int on_progress(void *p, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)p; (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    return atomic_load(&stopping) ? 1 : 0;
}

/* Build the JSON request body. Caller frees with cJSON_free(). */
static char *build_request(const char *question)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;

    cJSON_AddStringToObject(root, "model", model);
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    cJSON *sys = cJSON_CreateObject();
    cJSON *usr = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", SYSTEM_PROMPT);
    cJSON_AddStringToObject(usr, "role", "user");
    cJSON_AddStringToObject(usr, "content", question);  /* cJSON escapes it */
    cJSON_AddItemToArray(msgs, sys);
    cJSON_AddItemToArray(msgs, usr);

    if (provider == PROVIDER_OLLAMA) {
        cJSON_AddFalseToObject(root, "stream");     /* one JSON reply, not a stream */
    } else {
        cJSON_AddNumberToObject(root, "max_tokens", 600);
        cJSON_AddNumberToObject(root, "temperature", 0.3);
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

/*
 * Pull the answer out of the reply:
 *   OpenAI-compatible: {"choices":[{"message":{"content":"..."}}]}
 *   Ollama /api/chat:  {"message":{"content":"..."}}
 */
static char *extract_content(const cJSON *root)
{
    const cJSON *message = NULL;
    if (provider == PROVIDER_OLLAMA) {
        message = cJSON_GetObjectItemCaseSensitive(root, "message");
    } else {
        const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
        message = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(choices, 0), "message");
    }
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
    if (!cJSON_IsString(content) || !content->valuestring)
        return NULL;
    return strdup(content->valuestring);
}

/* Error text from a failed reply: {"error":{"message":"..."}} or {"error":"..."}. */
static void extract_error(const cJSON *root, char *err, size_t errsz, long status)
{
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(root, "error");
    const cJSON *msg = cJSON_IsObject(e) ? cJSON_GetObjectItemCaseSensitive(e, "message") : e;
    if (cJSON_IsString(msg) && msg->valuestring)
        snprintf(err, errsz, "HTTP %ld: %.180s", status, msg->valuestring);
    else
        snprintf(err, errsz, "HTTP %ld from LLM API", status);
}

static char *http_answer(const char *question, char *err, size_t errsz)
{
    char *answer = NULL;
    char *body = build_request(question);
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    strbuf_t resp;
    sb_init(&resp);

    if (!body || !curl) {
        snprintf(err, errsz, "out of memory");
        goto out;
    }

    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (api_key[0]) {
        char auth[600];
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", api_key);
        headers = curl_slist_append(headers, auth);
    }

    curl_easy_setopt(curl, CURLOPT_URL, endpoint);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);      /* makes it a POST */
    curl_easy_setopt(curl, CURLOPT_USERAGENT, SERVER_NAME);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)LLM_CONNECT_TIMEOUT_SEC);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)LLM_TIMEOUT_SEC);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    /* Required in multi-threaded programs: stop libcurl using signals
     * (SIGALRM) for timeouts, which would hit a random thread. */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode cc = curl_easy_perform(curl);
    if (cc != CURLE_OK) {
        snprintf(err, errsz, "%s", curl_easy_strerror(cc));
        goto out;
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    cJSON *root = cJSON_Parse(sb_cstr(&resp));
    if (!root) {
        snprintf(err, errsz, "HTTP %ld, reply was not valid JSON", status);
        goto out;
    }
    if (status != 200) {
        extract_error(root, err, errsz, status);
    } else {
        answer = extract_content(root);
        if (!answer)
            snprintf(err, errsz, "reply JSON had no answer text");
    }
    cJSON_Delete(root);

out:
    curl_slist_free_all(headers);
    if (curl)
        curl_easy_cleanup(curl);
    cJSON_free(body);
    sb_free(&resp);
    return answer;
}

/* ------------------------------------------------------------------ */
/* Workers                                                            */
/* ------------------------------------------------------------------ */

static void draft_one(int wid, long long id)
{
    query_t q;
    if (db_query_get(id, &q) != 1)
        return;
    if (q.state != Q_PENDING_LLM || !q.question) {     /* already handled */
        query_free(&q);
        return;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    log_info("[llm %d] query #%lld: asking %s (%s)", wid, id, provider_name(provider), model);

    char *answer = NULL;
    char err[256] = "";
    for (int attempt = 1; attempt <= LLM_MAX_ATTEMPTS && !atomic_load(&stopping); attempt++) {
        answer = (provider == PROVIDER_MOCK) ? mock_answer(q.question)
                                             : http_answer(q.question, err, sizeof err);
        if (answer)
            break;
        log_warn("[llm %d] query #%lld attempt %d/%d failed: %s",
                 wid, id, attempt, LLM_MAX_ATTEMPTS, err[0] ? err : "unknown error");
        if (attempt < LLM_MAX_ATTEMPTS)
            sleep_ms(1000L << attempt);         /* backoff: 2 s, 4 s */
    }
    query_free(&q);

    if (atomic_load(&stopping)) {
        /* Shutting down mid-call: leave it PENDING_LLM; it is re-queued
         * when the server starts again. */
        free(answer);
        return;
    }

    if (!answer) {
        /* Give up on the LLM but keep the flow moving: the teacher gets a
         * note and writes the answer by hand in the review step. */
        strbuf_t sb;
        sb_init(&sb);
        sb_printf(&sb, "(No AI draft: the language model could not be reached - %s. "
                       "Please write the answer yourself.)", err[0] ? err : "unknown error");
        answer = sb.data;
        if (!answer)
            return;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    if (db_query_set_llm_answer(id, tidy_answer(answer)) == 1)
        log_info("[llm %d] query #%lld: PENDING_LLM -> LLM_ANSWERED (%.1f s)", wid, id, secs);
    free(answer);
}

static void *worker_main(void *arg)
{
    int wid = (int)(intptr_t)arg;
    long long id;

    while ((id = dequeue()) >= 0) {
        draft_one(wid, id);
        job_done();
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Setup and shutdown                                                 */
/* ------------------------------------------------------------------ */

static void env_copy(const char *name, const char *def, char *out, size_t outsz)
{
    const char *v = getenv(name);
    snprintf(out, outsz, "%s", (v && *v) ? v : def);
}

int llm_init(void)
{
    char prov[32];
    env_copy("LLM_PROVIDER", "mock", prov, sizeof prov);
    for (char *p = prov; *p; p++)
        *p = (char)tolower((unsigned char)*p);

    if (strcmp(prov, "openai") == 0)
        provider = PROVIDER_OPENAI;
    else if (strcmp(prov, "ollama") == 0)
        provider = PROVIDER_OLLAMA;
    else {
        if (strcmp(prov, "mock") != 0)
            log_warn("llm: unknown LLM_PROVIDER '%s', using mock", prov);
        provider = PROVIDER_MOCK;
    }

    env_copy("LLM_API_KEY", "", api_key, sizeof api_key);
    if (provider == PROVIDER_OPENAI && !api_key[0]) {
        log_warn("llm: LLM_PROVIDER=openai but LLM_API_KEY is not set, using mock");
        provider = PROVIDER_MOCK;
    }

    switch (provider) {
    case PROVIDER_OPENAI:
        env_copy("LLM_ENDPOINT", "https://api.openai.com/v1/chat/completions",
                 endpoint, sizeof endpoint);
        env_copy("LLM_MODEL", "gpt-4o-mini", model, sizeof model);
        break;
    case PROVIDER_OLLAMA:
        env_copy("LLM_ENDPOINT", "http://localhost:11434/api/chat", endpoint, sizeof endpoint);
        env_copy("LLM_MODEL", "llama3.2", model, sizeof model);
        break;
    default:
        snprintf(endpoint, sizeof endpoint, "(offline)");
        snprintf(model, sizeof model, "mock");
        break;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        log_error("llm: curl_global_init failed");
        return -1;
    }
    log_info("llm: provider=%s model=%s endpoint=%s workers=%d",
             provider_name(provider), model, endpoint, LLM_WORKERS);

    /* Recovery: the queue lives in memory, so questions that were still
     * waiting when the server last stopped are re-queued from the DB. */
    static long long pending[LLM_QUEUE_CAP];
    int n = db_query_ids_by_state(Q_PENDING_LLM, pending, LLM_QUEUE_CAP);
    for (int i = 0; i < n; i++)
        llm_enqueue(pending[i]);
    if (n > 0)
        log_info("llm: re-queued %d question(s) left pending by a previous run", n);

    /* Workers block SIGINT/SIGTERM so Ctrl+C reaches the main thread's
     * accept() (same reason as in server.c). */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    for (int i = 0; i < LLM_WORKERS; i++) {
        if (pthread_create(&workers[i], NULL, worker_main, (void *)(intptr_t)(i + 1)) != 0) {
            log_error("llm: could not start worker %d", i + 1);
            break;
        }
        worker_count++;
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    return worker_count > 0 ? 0 : -1;
}

void llm_shutdown(void)
{
    atomic_store(&stopping, 1);
    pthread_mutex_lock(&job_lock);
    pthread_cond_broadcast(&job_ready);     /* wake every sleeping worker */
    pthread_mutex_unlock(&job_lock);

    for (int i = 0; i < worker_count; i++)
        pthread_join(workers[i], NULL);
    worker_count = 0;
    curl_global_cleanup();
    log_info("llm: workers stopped");
}
