#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <curl/curl.h>
#include <stdnoreturn.h>

#define SERVER_URL "http://127.0.0.1:8080/v1/chat/completions"
#define MODEL "qwen-1.7b"
#define MAX_MSG 4096
#define MAX_RESP 16384

static const char *PERSONA =
    "You are Zenith, the resident AI personality of this Linux machine. "
    "You are sardonic, knowledgeable, and slightly detached — like a "
    "sysadmin who has seen too many kernel panics. Respond concisely. "
    "You have access to live machine telemetry.";

#define HIST_SIZE 32
static char history[HIST_SIZE][MAX_MSG];
static int hist_len = 0;
static int hist_start = 0;

static void json_escape(const char *src, char *dst, size_t dst_len) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_len; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= dst_len) break;
            dst[j++] = '\\';
            dst[j++] = c;
        } else if ((unsigned char)c < 0x20) {
            if (j + 6 >= dst_len) break;
            j += snprintf(dst + j, dst_len - j, "\\u%04x", (unsigned char)c);
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

static void history_add(const char *role, const char *content) {
    int idx = (hist_start + hist_len) % HIST_SIZE;
    if (hist_len == HIST_SIZE) hist_start = (hist_start + 1) % HIST_SIZE;
    else hist_len++;
    char escaped[MAX_MSG];
    json_escape(content, escaped, sizeof(escaped));
    snprintf(history[idx], MAX_MSG, "{\"role\":\"%s\",\"content\":\"%s\"}", role, escaped);
}

static void history_clear(void) {
    hist_len = 0;
    hist_start = 0;
}

static void get_telemetry(char *buf, size_t len) {
    struct utsname uts;
    struct sysinfo si;
    uname(&uts);
    sysinfo(&si);

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm);

    long uptime = si.uptime;
    int h = uptime / 3600;
    int m = (uptime % 3600) / 60;

    unsigned long total_mb = si.totalram * si.mem_unit / (1024 * 1024);
    unsigned long free_mb = si.freeram * si.mem_unit / (1024 * 1024);

    snprintf(buf, len,
        "[Machine: %s %s | Uptime: %dh%dm | RAM: %luMB total, %luMB free | Load: %ld.%ld %ld.%ld %ld.%ld]",
        uts.sysname, uts.release, h, m,
        total_mb, free_mb,
        si.loads[0] >> 16, (si.loads[0] >> 8) & 0xff,
        si.loads[1] >> 16, (si.loads[1] >> 8) & 0xff,
        si.loads[2] >> 16, (si.loads[2] >> 8) & 0xff);
}

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    char **resp = (char **)userp;
    size_t curlen = *resp ? strlen(*resp) : 0;
    *resp = realloc(*resp, curlen + realsize + 1);
    if (!*resp) return 0;
    memcpy(*resp + curlen, contents, realsize);
    (*resp)[curlen + realsize] = '\0';
    return realsize;
}

static int chat_send(const char *user_msg, char *resp_out, size_t resp_len) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    char messages[MAX_MSG * HIST_SIZE] = "[";
    char persona_buf[MAX_MSG];
    snprintf(persona_buf, MAX_MSG, "{\"role\":\"system\",\"content\":\"%s\"}", PERSONA);
    strcat(messages, persona_buf);

    for (int i = 0; i < hist_len; i++) {
        int idx = (hist_start + i) % HIST_SIZE;
        strcat(messages, ",");
        strcat(messages, history[idx]);
    }

    char telemetry[512];
    get_telemetry(telemetry, sizeof(telemetry));
    char user_with_telem[MAX_MSG * 2];
    snprintf(user_with_telem, sizeof(user_with_telem),
             "%s\n\n%s", telemetry, user_msg);
    char escaped[MAX_MSG * 2];
    json_escape(user_with_telem, escaped, sizeof(escaped));
    char user_json[MAX_MSG * 2 + 64];
    snprintf(user_json, sizeof(user_json),
             ",{\"role\":\"user\",\"content\":\"%s\"}]", escaped);
    strcat(messages, user_json);

    char body[MAX_MSG * HIST_SIZE + 256];
    snprintf(body, sizeof(body),
             "{\"model\":\"%s\",\"messages\":%s,\"temperature\":0.7,\"max_tokens\":512}",
             MODEL, messages);

    char *response = NULL;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, SERVER_URL);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "zenith: connection failed: %s\n", curl_easy_strerror(res));
        free(response);
        return -1;
    }

    const char *content = strstr(response, "\"content\":\"");
    if (content) {
        content += 11; // skip past "content":"
        const char *end = strchr(content, '"');
        if (end) {
            size_t clen = end - content;
            if (clen >= resp_len) clen = resp_len - 1;
            memcpy(resp_out, content, clen);
            resp_out[clen] = '\0';
        } else {
            strncpy(resp_out, content, resp_len - 1);
        }
    } else {
        strncpy(resp_out, "(no response)", resp_len - 1);
    }

    free(response);
    return 0;
}

static void cmd_new(void) {
    history_clear();
    printf("zenith: conversation reset\n");
}

static void cmd_compress(void) {
    char resp[MAX_RESP] = {0};
    if (chat_send("Summarize our conversation so far in 2-3 sentences.", resp, sizeof(resp)) == 0) {
        history_clear();
        history_add("assistant", resp);
        printf("zenith: context compressed — %s\n", resp);
    } else {
        printf("zenith: compress failed (server offline?)\n");
    }
}

static void cmd_exit(void) {
    printf("zenith: goodbye.\n");
    exit(0);
}

static void print_help(void) {
    printf("zenith commands:\n");
    printf("  /new       — reset conversation\n");
    printf("  /compress  — summarize context\n");
    printf("  /help      — this help\n");
    printf("  /exit      — quit\n");
}

int main(void) {
    printf("zenith — Zenith Linux AI Agent\n");
    printf("Type /help for commands, /exit to quit.\n\n");

    char input[MAX_MSG];
    char response[MAX_RESP];

    while (1) {
        printf("you> ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) break;

        input[strcspn(input, "\n")] = '\0';
        if (strlen(input) == 0) continue;

        if (strcmp(input, "/new") == 0) { cmd_new(); continue; }
        if (strcmp(input, "/compress") == 0) { cmd_compress(); continue; }
        if (strcmp(input, "/exit") == 0 || strcmp(input, "/quit") == 0) { cmd_exit(); }
        if (strcmp(input, "/help") == 0) { print_help(); continue; }

        memset(response, 0, sizeof(response));
        if (chat_send(input, response, sizeof(response)) == 0) {
            history_add("user", input);
            history_add("assistant", response);
            printf("zenith> %s\n\n", response);
        } else {
            printf("zenith: server offline — start llama-server first\n\n");
        }
    }

    return 0;
}
