/*
 * llm.h - Drafting answers with a Large Language Model.
 *
 * Here our server is itself an HTTP *client*: it sends an HTTP POST with
 * a JSON body to an LLM API (using libcurl) and parses the JSON reply
 * (using cJSON). Chosen at run time with environment variables:
 *
 *   LLM_PROVIDER  mock (default) | openai | ollama
 *   LLM_ENDPOINT  full URL of the chat endpoint (optional, has defaults)
 *   LLM_MODEL     model name (optional, has defaults)
 *   LLM_API_KEY   bearer token (needed for openai; never logged)
 *
 *   mock    no network at all; returns a canned draft after a short delay,
 *           so the whole system works offline.
 *   openai  any OpenAI-compatible /chat/completions API (OpenAI, Groq,
 *           OpenRouter, Gemini's OpenAI endpoint, ...).
 *   ollama  a local Ollama server, default http://localhost:11434/api/chat
 *
 * Flow: the web handler for POST /ask saves the question (PENDING_LLM) and
 * calls llm_enqueue(id). A pool of LLM_WORKERS background threads takes
 * ids from the queue, calls the LLM, and stores the draft, moving the
 * question to LLM_ANSWERED. The student's page polls and sees the change.
 */
#ifndef LLM_H
#define LLM_H

typedef struct {
    const char *provider;   /* "mock", "openai", "ollama" */
    const char *model;
    const char *endpoint;
    int         queued;     /* jobs waiting in the queue */
    int         busy;       /* workers currently calling the LLM */
    int         workers;
} llm_status_t;

/*
 * Read the configuration, initialise libcurl, re-queue questions left in
 * PENDING_LLM by a previous run, and start the worker threads. Must be
 * called from the main thread before any other thread exists
 * (curl_global_init is not thread-safe). 0 on success.
 */
int  llm_init(void);

/* Stop the workers (aborting any in-flight request) and clean up. */
void llm_shutdown(void);

/* Queue a question for drafting. 0 on success, -1 if the queue is full
 * (the question stays PENDING_LLM and is picked up on the next restart). */
int  llm_enqueue(long long query_id);

void llm_get_status(llm_status_t *out);

#endif /* LLM_H */
