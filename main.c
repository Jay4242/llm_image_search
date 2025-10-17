#define _GNU_SOURCE
#include "raylib.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <curl/curl.h>
#include <jansson.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

static bool filesLoaded = false;
static FilePathList files = {0};
static int selectedIndex = -1;
static Texture2D image = {0};
static int leftPanelWidth = 500; // mutable width, can be resized by user
static int scrollOffset = 0; // vertical scroll offset for file list
static bool resizingPanel = false;
static int resizeStartX = 0;
static int originalPanelWidth = 0;

static bool has_image_extension(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (!ext) return false;
    ext++; // skip dot
    if (strcasecmp(ext, "png") == 0) return true;
    if (strcasecmp(ext, "jpg") == 0) return true;
    if (strcasecmp(ext, "jpeg") == 0) return true;
    if (strcasecmp(ext, "gif") == 0) return true;
    if (strcasecmp(ext, "bmp") == 0) return true;
    if (strcasecmp(ext, "webp") == 0) return true;
    return false;
}

/* -------------------------------------------------
   Recursive directory loader (replaces LoadDirectoryFilesEx)
   ------------------------------------------------- */
static FilePathList load_files_recursive(const char *basePath)
{
    FilePathList list = {0};
    size_t capacity = 0;

    /* Helper to add a path to the list */
    void add_path(const char *path)
    {
        if (list.count >= capacity)
        {
            capacity = capacity ? capacity * 2 : 128;
            list.paths = realloc(list.paths, capacity * sizeof(char *));
        }
        list.paths[list.count++] = strdup(path);
    }

    /* Recursive scan */
    void scan_dir(const char *dir)
    {
        DIR *d = opendir(dir);
        if (!d) return;
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL)
        {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;

            char *full = NULL;
            if (asprintf(&full, "%s/%s", dir, entry->d_name) == -1) continue;

            struct stat st;
            if (stat(full, &st) == -1) { free(full); continue; }

            if (S_ISDIR(st.st_mode))
            {
                scan_dir(full);
                free(full);
            }
            else
            {
                add_path(full);
                free(full);
            }
        }
        closedir(d);
    }

    scan_dir(basePath);
    return list;
}

/* -------------------------------------------------
   LLM interaction helpers (generic POST request)
   ------------------------------------------------- */

static const char *LLM_SERVER_URL = "http://localhost:9090/v1/chat/completions";

/* Structure to hold response data from libcurl */
typedef struct {
    char *data;
    size_t size;
} ResponseData;

typedef struct {
    char *prompt;
    char *b64;
    double temperature;
} llm_task;

static pthread_mutex_t llm_mutex = PTHREAD_MUTEX_INITIALIZER;
static char *llm_response = NULL;
static bool llm_response_ready = false;
static volatile sig_atomic_t keep_running = 1;
static bool batch_search_active = false;
static int batch_search_index = 0;
static bool stop_requested = false;
static bool loading = false;
static pthread_t loader_thread;
static pthread_mutex_t files_mutex = PTHREAD_MUTEX_INITIALIZER;

static void handle_sigint(int sig)
{
    (void)sig; // suppress unused parameter warning
    keep_running = 0;
}

/* libcurl write callback to accumulate response */
static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t total = size * nmemb;
    ResponseData *resp = (ResponseData *)userdata;
    char *new_data = realloc(resp->data, resp->size + total + 1);
    if (!new_data) return 0; /* allocation failed */
    memcpy(new_data + resp->size, ptr, total);
    new_data[resp->size + total] = '\0';
    resp->data = new_data;
    resp->size += total;
    return total;
}

/* -------------------------------------------------
   Backspace handling helpers with repeat logic
   ------------------------------------------------- */

/* Directory path backspace handling */
static void handle_backspace(char *buffer)
{
    static int repeatCounter = 0;
    if (IsKeyDown(KEY_BACKSPACE))
    {
        repeatCounter++;
        if (repeatCounter == 1 || repeatCounter % 5 == 0)
        {
            size_t len = strlen(buffer);
            if (len > 0) buffer[len - 1] = '\0';
        }
    }
    else
    {
        repeatCounter = 0;
    }
}

/* -------------------------------------------------
   Helper: alphanumeric sort for file list
   ------------------------------------------------- */

/* Custom character ranking:
      0-9  -> 0-9
      a/A -> 10,11
      b/B -> 12,13
      ...  -> continue */
static int char_rank(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return 10 + (c - 'a') * 2;
    if (c >= 'A' && c <= 'Z')
        return 10 + (c - 'A') * 2 + 1;
    return 1000 + c;  /* fallback for other characters */
}

/* Comparator using the custom ranking */
static int cmp_strings(const void *a, const void *b)
{
    const char * const *sa = (const char * const *)a;
    const char * const *sb = (const char * const *)b;
    const unsigned char *s1 = (const unsigned char *)*sa;
    const unsigned char *s2 = (const unsigned char *)*sb;

    while (*s1 && *s2) {
        int r1 = char_rank(*s1);
        int r2 = char_rank(*s2);
        if (r1 != r2) return r1 - r2;
        s1++; s2++;
    }
    return char_rank(*s1) - char_rank(*s2);
}

/* Search phrase backspace handling */

/* -------------------------------------------------
   Simple Base64 encoder (no line breaks)
   ------------------------------------------------- */
static const char b64_encoding_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const int b64_mod_table[] = {0, 2, 1};

static char *base64_encode(const unsigned char *data, size_t input_length)
{
    size_t output_length = 4 * ((input_length + 2) / 3);
    char *encoded_data = malloc(output_length + 1);
    if (encoded_data == NULL) return NULL;

    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? data[i++] : 0;
        uint32_t octet_b = i < input_length ? data[i++] : 0;
        uint32_t octet_c = i < input_length ? data[i++] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        encoded_data[j++] = b64_encoding_table[(triple >> 18) & 0x3F];
        encoded_data[j++] = b64_encoding_table[(triple >> 12) & 0x3F];
        encoded_data[j++] = b64_encoding_table[(triple >> 6) & 0x3F];
        encoded_data[j++] = b64_encoding_table[triple & 0x3F];
    }

    for (int i = 0; i < b64_mod_table[input_length % 3]; i++)
        encoded_data[output_length - 1 - i] = '=';

    encoded_data[output_length] = '\0';
    return encoded_data;
}

/*
 * Sends a chat completion request to the LLM backend.
 * `prompt` – the user message to send.
 * `temperature` – sampling temperature (e.g., 0.7).
 * Returns a newly allocated string containing the raw JSON response,
 * or NULL on failure. Caller must free() the returned pointer.
 */
static char *getLLMResponse(const char *prompt, const char *base64_image, double temperature)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "curl_easy_init() failed\n");
        return NULL;
    }

    /* Build JSON payload */
    char *payload = NULL;
    if (asprintf(&payload,
                 "{\"model\": \"gpt-4-vision-preview\", \"messages\": [{\"role\": \"system\", \"content\": \"You are a helpful assistant.\"}, {\"role\": \"user\", \"content\": [{\"type\": \"text\", \"text\": \"%s\"}, {\"type\": \"image_url\", \"image_url\": {\"url\": \"data:image/jpeg;base64,%s\"}}]}], \"temperature\": %f}",
                 prompt, base64_image, temperature) == -1) {
        fprintf(stderr, "Failed to allocate payload string\n");
        curl_easy_cleanup(curl);
        return NULL;
    }

    ResponseData resp = {NULL, 0};

    curl_easy_setopt(curl, CURLOPT_URL, LLM_SERVER_URL);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1800L);
    /* Optional: disable SSL verification if using self‑signed certs */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    /* Disable Expect: 100‑continue to avoid server rejecting large payloads */
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Expect:");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        free(payload);
        curl_easy_cleanup(curl);
        if (resp.data) free(resp.data);
        if (headers) curl_slist_free_all(headers);
        return NULL;
    }

    free(payload);
    curl_easy_cleanup(curl);
    if (headers) curl_slist_free_all(headers);
    return resp.data;   /* Caller must free */
}

/* -------------------------------------------------
   Thread worker for LLM request
   ------------------------------------------------- */
static void *llm_thread_func(void *arg)
{
    llm_task *task = (llm_task *)arg;
    char *response = getLLMResponse(task->prompt, task->b64, task->temperature);
    free(task->prompt);
    free(task->b64);

    pthread_mutex_lock(&llm_mutex);
    if (llm_response) free(llm_response);
    llm_response = response;
    llm_response_ready = true;
    pthread_mutex_unlock(&llm_mutex);

    free(task);
    return NULL;
}

/* Helper to start an LLM request for a given image file */
static void start_llm_task_for_file(const char *filepath, const char *search_phrase)
{
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        fprintf(stderr, "Failed to open image file: %s\n", filepath);
        return;
    }
    printf("Processing image: %s\n", filepath);
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char *buf = malloc(fsize);
    if (!buf) {
        fclose(fp);
        return;
    }
    size_t read_bytes = fread(buf, 1, fsize, fp);
    fclose(fp);
    if (read_bytes != (size_t)fsize) {
        fprintf(stderr, "Failed to read file %s\n", filepath);
        free(buf);
        return;
    }

    char *b64 = base64_encode(buf, fsize);
    free(buf);
    if (!b64) return;

    char *prompt = NULL;
    if (asprintf(&prompt, "Does the image contain %s?", search_phrase) == -1) {
        free(b64);
        return;
    }

    llm_task *task = malloc(sizeof(llm_task));
    if (!task) {
        free(prompt);
        free(b64);
        return;
    }
    task->prompt = prompt;
    task->b64 = b64;
    task->temperature = 0.0;

    pthread_t tid;
    pthread_create(&tid, NULL, llm_thread_func, task);
    pthread_detach(tid);
}

/* -------------------------------------------------
   Thread worker for loading directory files (non‑blocking)
   ------------------------------------------------- */
struct load_task {
    char dir[256];
    bool recursive;
};

static void *load_files_thread(void *arg)
{
    struct load_task *task = (struct load_task *)arg;
    FilePathList newlist = {0};

    if (task->recursive)
        newlist = load_files_recursive(task->dir);
    else
        newlist = LoadDirectoryFiles(task->dir);

    if (newlist.count > 1)
        qsort(newlist.paths, newlist.count, sizeof(char *), cmp_strings);

    pthread_mutex_lock(&files_mutex);
    if (filesLoaded)
        UnloadDirectoryFiles(files);
    files = newlist;
    filesLoaded = true;
    loading = false;
    pthread_mutex_unlock(&files_mutex);

    free(task);
    return NULL;
}

int main(void)
{
    // Initialization
    const int screenWidth = 800;
    const int screenHeight = 450;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(screenWidth, screenHeight, "LLM Image Search");

    SetTargetFPS(60);

    // -------------------------------------------------
    // UI state variables
    // -------------------------------------------------
    char dirPath[256] = "";
    bool editingDir = false;
    bool recursive = false;
    const int checkboxSize = 20;
    const int inputBoxHeight = 30;
    Rectangle inputBox; // UI input rectangle, declared once for reuse
    const int buttonWidth = 100;
    const int searchBarWidth = 400;
    char searchPhrase[256] = "";
    bool editingSearch = false;
    int searchScrollOffset = 0;

    // Register SIGINT handler for clean exit
    signal(SIGINT, handle_sigint);

    // Main game loop
    while (!WindowShouldClose() && keep_running)
    {
        // -------------------------------------------------
        // Input handling
        // -------------------------------------------------
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
        {
            Vector2 mouse = GetMousePosition();

            // Input box for directory path
            Rectangle inputBox = {10, 10, (float)(GetScreenWidth() - 20 - buttonWidth - 10), (float)inputBoxHeight};
            if (CheckCollisionPointRec(mouse, inputBox))
                editingDir = true;
            else
                editingDir = false;

             // Load button
             Rectangle loadBtn = {inputBox.x + inputBox.width + 10, 10, (float)buttonWidth, (float)inputBoxHeight};
             if (CheckCollisionPointRec(mouse, loadBtn))
             {
                 /* Cancel any previous load and start a new background load */
                 if (filesLoaded)
                 {
                     UnloadDirectoryFiles(files);
                     filesLoaded = false;
                 }
                 if (!loading)
                 {
                     loading = true;
                     struct load_task *task = malloc(sizeof(*task));
                     strncpy(task->dir, dirPath, sizeof(task->dir) - 1);
                     task->dir[sizeof(task->dir) - 1] = '\0';
                     task->recursive = recursive;
                     pthread_create(&loader_thread, NULL, load_files_thread, task);
                     pthread_detach(loader_thread);
                 }

                 selectedIndex = -1;
                 if (image.id != 0) { UnloadTexture(image); image.id = 0; }
             }

            // Recursive checkbox
            Rectangle checkBox = {10, inputBox.y + inputBox.height + 10, (float)checkboxSize, (float)checkboxSize};
            if (CheckCollisionPointRec(mouse, checkBox))
                recursive = !recursive;

            // Search bar input handling (right justified)
            int stopBtnWidth = 80;
            int spacing = 10;
            int effectiveSearchBarWidth = searchBarWidth;
            if (batch_search_active) {
                effectiveSearchBarWidth = searchBarWidth - (stopBtnWidth + spacing);
                if (effectiveSearchBarWidth < 50) effectiveSearchBarWidth = 50;
            }
            Rectangle searchBox = { (float)(GetScreenWidth() - buttonWidth - effectiveSearchBarWidth - 20), checkBox.y, (float)effectiveSearchBarWidth, (float)inputBoxHeight };
            Rectangle searchBtn = { (float)(GetScreenWidth() - buttonWidth - 10), checkBox.y, (float)buttonWidth, (float)inputBoxHeight };
            Rectangle stopBtn = { searchBtn.x - stopBtnWidth - spacing, checkBox.y, (float)stopBtnWidth, (float)inputBoxHeight };
            if (CheckCollisionPointRec(mouse, searchBox))
                editingSearch = true;
            else if (editingSearch && !CheckCollisionPointRec(mouse, searchBox))
                editingSearch = false;

            // Search button (placeholder)
            if (CheckCollisionPointRec(mouse, searchBtn))
            {
                if (!batch_search_active) {
                    /* Start batch search over all image files */
                    batch_search_active = true;
                    stop_requested = false;
                    batch_search_index = 0;
                    /* Find first image file */
                    while (batch_search_index < (int)files.count &&
                           !has_image_extension(files.paths[batch_search_index])) {
                        batch_search_index++;
                    }
                    if (batch_search_index < (int)files.count) {
                        start_llm_task_for_file(files.paths[batch_search_index], searchPhrase);
                    } else {
                        batch_search_active = false; /* no images */
                    }
                }
            }
            /* Stop button handling */
            if (batch_search_active && CheckCollisionPointRec(mouse, stopBtn)) {
                stop_requested = true;
                batch_search_active = false;
            }
        }

        // -------------------------------------------------
        // Additional UI interactions: scrolling and panel resizing
        // -------------------------------------------------
        // Paste clipboard into directory path on right-click
        if (IsMouseButtonPressed(MOUSE_RIGHT_BUTTON))
        {
            Vector2 mouse = GetMousePosition();
            Rectangle inputBox = {10, 10, (float)(GetScreenWidth() - 20 - buttonWidth - 10), (float)inputBoxHeight};
            if (CheckCollisionPointRec(mouse, inputBox))
            {
                const char *clip = GetClipboardText();
                if (clip)
                {
                    strncpy(dirPath, clip, sizeof(dirPath) - 1);
                    dirPath[sizeof(dirPath) - 1] = '\0';
                }
            }
        }

        // Paste clipboard into search box on right-click
        if (IsMouseButtonPressed(MOUSE_RIGHT_BUTTON))
        {
            Vector2 mouse = GetMousePosition();
            // Recompute search box rectangle (same logic as UI drawing)
            int stopBtnWidth = 80;
            int spacing = 10;
            int effectiveSearchBarWidth = searchBarWidth;
            if (batch_search_active) {
                effectiveSearchBarWidth = searchBarWidth - (stopBtnWidth + spacing);
                if (effectiveSearchBarWidth < 50) effectiveSearchBarWidth = 50;
            }
            Rectangle searchBox = { (float)(GetScreenWidth() - buttonWidth - effectiveSearchBarWidth - 20), checkBox.y, (float)effectiveSearchBarWidth, (float)inputBoxHeight };
            if (CheckCollisionPointRec(mouse, searchBox))
            {
                const char *clip = GetClipboardText();
                if (clip)
                {
                    strncpy(searchPhrase, clip, sizeof(searchPhrase) - 1);
                    searchPhrase[sizeof(searchPhrase) - 1] = '\0';
                }
            }
        }
        // Scroll handling for file list (mouse wheel)
        inputBox = (Rectangle){10, 10, (float)(GetScreenWidth() - 20 - buttonWidth - 10), (float)inputBoxHeight};
        if (filesLoaded && files.count > 0)
        {
            Rectangle panel = {0, (float)(inputBox.y + inputBox.height + 50), (float)leftPanelWidth, (float)(GetScreenHeight() - (inputBox.y + inputBox.height + 50))};
            if (CheckCollisionPointRec(GetMousePosition(), panel))
            {
                float wheel = GetMouseWheelMove();
                if (wheel != 0)
                {
                    // Adjust scroll offset (3 items per wheel step)
                    scrollOffset -= (int)wheel * 3;
                    if (scrollOffset < 0) scrollOffset = 0;
                }
            }
        }

        // Panel resizing (drag right edge of panel)
        {
            Rectangle resizeHandle = { (float)leftPanelWidth - 5, (float)(inputBox.y + inputBox.height + 50), 10, (float)(GetScreenHeight() - (inputBox.y + inputBox.height + 50)) };
            if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(GetMousePosition(), resizeHandle))
            {
                resizingPanel = true;
                resizeStartX = GetMouseX();
                originalPanelWidth = leftPanelWidth;
            }
            if (resizingPanel)
            {
                int delta = GetMouseX() - resizeStartX;
                leftPanelWidth = originalPanelWidth + delta;
                if (leftPanelWidth < 100) leftPanelWidth = 100;
                if (leftPanelWidth > GetScreenWidth() - 100) leftPanelWidth = GetScreenWidth() - 100;
                if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) resizingPanel = false;
            }
        }

        // Text input for directory path
        if (editingDir)
        {
            // Handle backspace with repeat while held down
            handle_backspace(dirPath);
            // Handle printable characters (including lowercase) using GetCharPressed
            int ch = GetCharPressed();
            while (ch > 0)
            {
                if (ch >= 32 && ch <= 126 && strlen(dirPath) < sizeof(dirPath) - 1)
                {
                    size_t len = strlen(dirPath);
                    dirPath[len] = (char)ch;
                    dirPath[len + 1] = '\0';
                }
                ch = GetCharPressed();
            }
        }

        // Text input for search phrase
        if (editingSearch)
        {
            // Handle backspace with repeat while held down
            handle_backspace(searchPhrase);
            // Handle printable characters
            int ch = GetCharPressed();
            while (ch > 0)
            {
                if (ch >= 32 && ch <= 126 && strlen(searchPhrase) < sizeof(searchPhrase) - 1)
                {
                    size_t len = strlen(searchPhrase);
                    searchPhrase[len] = (char)ch;
                    searchPhrase[len + 1] = '\0';
                }
                ch = GetCharPressed();
            }

            // Update scroll offset so the tail stays visible
            int maxChars = (searchBarWidth - 10) / 10; // rough estimate: 10px per character at font size 20
            if ((int)strlen(searchPhrase) > maxChars)
                searchScrollOffset = strlen(searchPhrase) - maxChars;
            else
                searchScrollOffset = 0;
        }

        // -------------------------------------------------
        // Drawing
        // -------------------------------------------------
        // Keyboard navigation for file list
        if (filesLoaded && files.count > 0) {
            if (IsKeyPressed(KEY_DOWN)) {
                int i = selectedIndex + 1;
                while (i < (int)files.count && !has_image_extension(files.paths[i])) i++;
                if (i < (int)files.count) {
                    selectedIndex = i;
                    if (image.id != 0) UnloadTexture(image);
                    image = LoadTexture(files.paths[i]);
                }
            } else if (IsKeyPressed(KEY_UP)) {
                int i = selectedIndex - 1;
                while (i >= 0 && !has_image_extension(files.paths[i])) i--;
                if (i >= 0) {
                    selectedIndex = i;
                    if (image.id != 0) UnloadTexture(image);
                    image = LoadTexture(files.paths[i]);
                }
            }
        }
        BeginDrawing();
        ClearBackground(RAYWHITE);

        // UI: Directory input
        inputBox = (Rectangle){10, 10, (float)(GetScreenWidth() - 20 - buttonWidth - 10), (float)inputBoxHeight};
        DrawRectangleRec(inputBox, LIGHTGRAY);
        DrawRectangleLinesEx(inputBox, 2, DARKGRAY);
        DrawText(dirPath, (int)inputBox.x + 5, (int)inputBox.y + 5, 20, BLACK);

        // UI: Load button
        Rectangle loadBtn = {inputBox.x + inputBox.width + 10, 10, (float)buttonWidth, (float)inputBoxHeight};
        DrawRectangleRec(loadBtn, GRAY);
        DrawRectangleLinesEx(loadBtn, 2, DARKGRAY);
        DrawText("Load", (int)loadBtn.x + 10, (int)loadBtn.y + 5, 20, WHITE);

        // UI: Recursive checkbox
        Rectangle checkBox = {10, inputBox.y + inputBox.height + 10, (float)checkboxSize, (float)checkboxSize};
        DrawRectangleRec(checkBox, LIGHTGRAY);
        DrawRectangleLinesEx(checkBox, 2, DARKGRAY);
        if (recursive) DrawText("X", (int)checkBox.x + 4, (int)checkBox.y + 2, 20, BLACK);
        DrawText("Recursive", (int)checkBox.x + checkboxSize + 5, (int)checkBox.y, 20, BLACK);

        // UI: Search bar (right justified)
        int stopBtnWidth = 80;
        int spacing = 10;
        int effectiveSearchBarWidth = searchBarWidth;
        if (batch_search_active) {
            effectiveSearchBarWidth = searchBarWidth - (stopBtnWidth + spacing);
            if (effectiveSearchBarWidth < 50) effectiveSearchBarWidth = 50;
        }
        Rectangle searchBox = { (float)(GetScreenWidth() - buttonWidth - effectiveSearchBarWidth - 20), checkBox.y, (float)effectiveSearchBarWidth, (float)inputBoxHeight };
        Rectangle searchBtn = { (float)(GetScreenWidth() - buttonWidth - 10), checkBox.y, (float)buttonWidth, (float)inputBoxHeight };
        Rectangle stopBtn = { searchBtn.x - stopBtnWidth - spacing, checkBox.y, (float)stopBtnWidth, (float)inputBoxHeight };
        DrawRectangleRec(searchBox, LIGHTGRAY);
        DrawRectangleLinesEx(searchBox, 2, DARKGRAY);
        DrawText(searchPhrase + searchScrollOffset, (int)searchBox.x + 5, (int)searchBox.y + 5, 20, BLACK);

        // UI: Search button
        DrawRectangleRec(searchBtn, GRAY);
        DrawRectangleLinesEx(searchBtn, 2, DARKGRAY);
        DrawText("Search", (int)searchBtn.x + 10, (int)searchBtn.y + 5, 20, WHITE);

        /* Stop button (visible during batch search) */
        if (batch_search_active) {
            DrawRectangleRec(stopBtn, RED);
            DrawRectangleLinesEx(stopBtn, 2, DARKGRAY);
            DrawText("Stop", (int)stopBtn.x + 10, (int)stopBtn.y + 5, 20, WHITE);
        }

        /* Stop button (visible during batch search) */
        if (batch_search_active) {
            DrawRectangleRec(stopBtn, RED);
            DrawRectangleLinesEx(stopBtn, 2, DARKGRAY);
            DrawText("Stop", (int)stopBtn.x + 10, (int)stopBtn.y + 5, 20, WHITE);
        }

        // UI: File list panel (scrollable and resizable)
        if (filesLoaded && files.count > 0)
        {
            Rectangle panel = {0, (float)(inputBox.y + inputBox.height + 50), (float)leftPanelWidth, (float)(GetScreenHeight() - (inputBox.y + inputBox.height + 50))};
            DrawRectangleRec(panel, LIGHTGRAY);
            DrawRectangleLinesEx(panel, 2, DARKGRAY);
            BeginScissorMode((int)panel.x, (int)panel.y, (int)panel.width, (int)panel.height);

            int maxVisible = (int)((panel.height - 10) / 25);
            int filteredIdx = -1;
            int totalFiltered = 0;

            // Count how many image files we have
            for (unsigned int i = 0; i < files.count; ++i)
            {
                if (has_image_extension(files.paths[i])) totalFiltered++;
            }

            // Clamp scroll offset to valid range
            if (scrollOffset > totalFiltered - maxVisible) scrollOffset = totalFiltered - maxVisible;
            if (scrollOffset < 0) scrollOffset = 0;

            int startY = (int)panel.y + 5;
            for (unsigned int i = 0; i < files.count; ++i)
            {
                if (!has_image_extension(files.paths[i])) continue;
                filteredIdx++;

                if (filteredIdx < scrollOffset) continue;
                if (filteredIdx >= scrollOffset + maxVisible) break;

                int drawIdx = filteredIdx - scrollOffset;
                Rectangle itemRect = {panel.x + 5, (float)(startY + drawIdx * 25), panel.width - 10, 24};
                if ((int)i == selectedIndex) DrawRectangleRec(itemRect, SKYBLUE);
                {
                    const char *displayName = files.paths[i];
                    size_t dirLen = strlen(dirPath);
                    if (dirLen > 0 && strncmp(displayName, dirPath, dirLen) == 0) {
                        const char *p = displayName + dirLen;
                        if (*p == '/' || *p == '\\') p++;
                        displayName = p;
                    }
                    DrawText(displayName, (int)itemRect.x + 2, (int)itemRect.y + 4, 20, BLACK);
                }

                // Selection handling
                if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
                {
                    Vector2 mouse = GetMousePosition();
                    if (CheckCollisionPointRec(mouse, itemRect))
                    {
                        if (selectedIndex != (int)i)
                        {
                            if (image.id != 0) UnloadTexture(image);
                            image = LoadTexture(files.paths[i]);
                        }
                        selectedIndex = i;
                    }
                }
                // Right‑click: copy full file path to clipboard
                if (IsMouseButtonPressed(MOUSE_RIGHT_BUTTON))
                {
                    Vector2 mouse = GetMousePosition();
                    if (CheckCollisionPointRec(mouse, itemRect))
                    {
                        SetClipboardText(files.paths[i]);
                    }
                }
            }

            EndScissorMode();
            // Draw scrollbar if needed
            if (totalFiltered > maxVisible)
            {
                const int sbWidth = 12;
                // Scrollbar background
                Rectangle sbBg = { panel.x + panel.width - sbWidth - 2, panel.y + 5, (float)sbWidth, panel.height - 10 };
                DrawRectangleRec(sbBg, LIGHTGRAY);
                // Compute thumb size and position
                float thumbHeight = ((float)maxVisible / totalFiltered) * (panel.height - 10);
                if (thumbHeight < 20) thumbHeight = 20;
                float thumbPos = 0.0f;
                if (totalFiltered - maxVisible > 0)
                    thumbPos = ((float)scrollOffset / (totalFiltered - maxVisible)) * ((panel.height - 10) - thumbHeight);
                Rectangle thumb = { sbBg.x, sbBg.y + thumbPos, (float)sbWidth, thumbHeight };
                DrawRectangleRec(thumb, DARKGRAY);
            }
            // Draw selected image on the right side (no up‑scaling)
            if (image.id != 0)
            {
                Rectangle destArea = {panel.width + 10, (float)(inputBox.y + inputBox.height + 50), (float)(GetScreenWidth() - panel.width - 20), (float)(GetScreenHeight() - (inputBox.y + inputBox.height + 50))};
                float scale = 1.0f;
                if (image.width > 0 && image.height > 0)
                {
                    float scaleX = destArea.width / image.width;
                    float scaleY = destArea.height / image.height;
                    scale = fmin(1.0f, fmin(scaleX, scaleY));
                }
                float drawW = image.width * scale;
                float drawH = image.height * scale;
                Rectangle src = {0, 0, (float)image.width, (float)image.height};
                Rectangle dst = {destArea.x + (destArea.width - drawW) / 2.0f,
                                 destArea.y + (destArea.height - drawH) / 2.0f,
                                 drawW, drawH};
                DrawTexturePro(image, src, dst, (Vector2){0,0}, 0.0f, WHITE);
            }
        }

        EndDrawing();

        /* -------------------------------------------------
           Process LLM response (if ready) on the main thread
           ------------------------------------------------- */
        pthread_mutex_lock(&llm_mutex);
        if (llm_response_ready)
        {
            json_error_t error;
            json_t *root = json_loads(llm_response, 0, &error);
            if (!root)
            {
                fprintf(stderr, "JSON parse error: %s\n", error.text);
            }
            else
            {
                json_t *choices = json_object_get(root, "choices");
                if (json_is_array(choices) && json_array_size(choices) > 0)
                {
                    json_t *first = json_array_get(choices, 0);
                    json_t *finish = json_object_get(first, "finish_reason");
                    json_t *message = json_object_get(first, "message");
                    const char *finish_reason = json_string_value(finish);
                    const char *content = NULL;
                    if (json_is_object(message))
                    {
                        json_t *content_obj = json_object_get(message, "content");
                        content = json_string_value(content_obj);
                    }
                    printf("Finish reason: %s\n", finish_reason ? finish_reason : "N/A");
                    printf("Assistant: %s\n", content ? content : "N/A");

                    /* Batch search handling */
                    if (batch_search_active) {
                        /* Determine keep/remove based on first word */
                        bool keep = true;
                        if (content) {
                            const char *p = content;
                            while (*p && isspace((unsigned char)*p)) p++;
                            if (strncasecmp(p, "yes", 3) == 0) {
                                keep = true;
                            } else if (strncasecmp(p, "no", 2) == 0) {
                                keep = false;
                            }
                        }

                        if (!keep && batch_search_index < (int)files.count) {
                            /* Remove current file */
                            free(files.paths[batch_search_index]);
                            for (int j = batch_search_index; j < (int)files.count - 1; ++j) {
                                files.paths[j] = files.paths[j + 1];
                            }
                            files.count--;
                            if (selectedIndex == batch_search_index) {
                                selectedIndex = -1;
                                if (image.id != 0) { UnloadTexture(image); image.id = 0; }
                            } else if (selectedIndex > batch_search_index) {
                                selectedIndex--;
                            }
                        }

                        /* Advance to next image */
                        if (keep) {
                            /* If we kept the file, move to the next index */
                            batch_search_index++;
                        }
                        /* If we removed the file, batch_search_index already points to the next item
                           because the list shifted left, so we do NOT increment. */

                        while (batch_search_index < (int)files.count &&
                               !has_image_extension(files.paths[batch_search_index])) {
                            batch_search_index++;
                        }

                        if (stop_requested || batch_search_index >= (int)files.count) {
                            batch_search_active = false;
                        } else {
                            /* Start next request */
                            start_llm_task_for_file(files.paths[batch_search_index], searchPhrase);
                        }
                    }
                }
                else
                {
                    fprintf(stderr, "Unexpected JSON structure: missing choices array\n");
                }
                json_decref(root);
            }
            free(llm_response);
            llm_response = NULL;
            llm_response_ready = false;
        }
        pthread_mutex_unlock(&llm_mutex);

    } // end while loop

    // De-Initialization
    if (image.id != 0) UnloadTexture(image);
    if (filesLoaded) UnloadDirectoryFiles(files);
    CloseWindow();

    return 0;
}
