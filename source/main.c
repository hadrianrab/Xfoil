#include <ctype.h>
#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <switch.h>
#include "cJSON.h"

#define DATA "sdmc:/switch/xfoil"
#define APP_VERSION "0.3.0"
#define DOWNLOADS DATA "/downloads"
#define RETENTION DATA "/retencao"
#define EXPIRY_SETTING DATA "/expirar-8h"
#define EXPIRY_SECONDS (8 * 60 * 60)
#define URL_CAP 1024
#define MAX_ITEMS 500
#define CATALOG_LIMIT (1024 * 1024)
#define FILE_LIMIT 4294967295LL

typedef struct {
    char title[97], game[97], requirement[97], file[97], url[URL_CAP], hash[65];
    curl_off_t size;
} Item;

typedef struct {
    FILE *file;
    curl_off_t remaining, offset, total;
    int last_percent;
    bool stopped;
} Transfer;

static PadState pad;
static Item *items;
static size_t count;
static char sources[4][URL_CAP], status[160], filter[97];
static bool expire_downloads;

#include "hud.inc"

static bool copy_field(char *out, size_t cap, const cJSON *object,
                       const char *key, bool required)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!value && !required) {
        out[0] = 0;
        return true;
    }
    if (!cJSON_IsString(value)) return false;
    size_t length = strlen(value->valuestring);
    if (!length || length >= cap) return false;
    for (size_t i = 0; i < length; ++i)
        if ((unsigned char)value->valuestring[i] < 32 ||
            (unsigned char)value->valuestring[i] == 127) return false;
    memcpy(out, value->valuestring, length + 1);
    return true;
}

static bool https_url(const char *url)
{
    if (strncmp(url, "https://", 8) || !url[8]) return false;
    size_t host = strcspn(url + 8, "/?#");
    return host && !memchr(url + 8, '@', host) && !strpbrk(url, "\\ \r\n\t");
}

static bool safe_file(const char *name)
{
    if (!isalnum((unsigned char)name[0]) || name[strlen(name) - 1] == '.') return false;
    for (; *name; ++name)
        if (!isalnum((unsigned char)*name) && !strchr("._-", *name)) return false;
    return true;
}

static bool title_package(const char *file)
{
    const char *ext = strrchr(file, '.');
    return ext && (!strcasecmp(ext, ".nsp") || !strcasecmp(ext, ".xci"));
}

static bool valid_clock(time_t now)
{
    return now >= 1577836800;
}

static void mark_for_expiry(const Item *item, const char *path)
{
    time_t now = time(NULL);
    struct stat info;
    if (!expire_downloads || !title_package(item->file) || !valid_clock(now) ||
        stat(path, &info) || !S_ISREG(info.st_mode))
        return;
    char marker[160];
    snprintf(marker, sizeof marker, RETENTION "/%s", item->hash);
    int fd = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return;
    FILE *file = fdopen(fd, "w");
    if (!file) {
        close(fd);
        remove(marker);
        return;
    }
    bool ok = fprintf(file, "%s\n%lld %lld %lld\n", item->file, (long long)info.st_size,
                      (long long)info.st_mtime, (long long)now + EXPIRY_SECONDS) > 0;
    if (fclose(file) || !ok) remove(marker);
}

static unsigned clear_expired(void)
{
    time_t now = time(NULL);
    if (!expire_downloads || !valid_clock(now)) return 0;
    DIR *directory = opendir(RETENTION);
    if (!directory) return 0;
    unsigned removed = 0;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (strlen(entry->d_name) != 64 || strspn(entry->d_name, "0123456789abcdef") != 64)
            continue;
        char marker[160], name[98], path[256];
        snprintf(marker, sizeof marker, RETENTION "/%.64s", entry->d_name);
        FILE *file = fopen(marker, "r");
        if (!file) continue;
        long long size, mtime, deadline;
        bool read = fgets(name, sizeof name, file) && strchr(name, '\n') &&
                    fscanf(file, "%lld %lld %lld", &size, &mtime, &deadline) == 3;
        fclose(file);
        if (!read || deadline < 1577836800 || deadline > now || size < 1) continue;
        name[strcspn(name, "\r\n")] = 0;
        if (!safe_file(name) || !title_package(name)) continue;
        snprintf(path, sizeof path, DOWNLOADS "/%s", name);
        struct stat info;
        if (stat(path, &info)) {
            if (errno == ENOENT) remove(marker);
            continue;
        }
        if (!S_ISREG(info.st_mode) || info.st_size != size || info.st_mtime != mtime)
            continue;
        if (!remove(path)) {
            remove(marker);
            ++removed;
        }
    }
    closedir(directory);
    return removed;
}

static void toggle_expiry(void)
{
    if (expire_downloads) {
        if (!remove(EXPIRY_SETTING) || errno == ENOENT) expire_downloads = false;
    } else {
        FILE *setting = fopen(EXPIRY_SETTING, "wb");
        if (setting && !fclose(setting)) expire_downloads = true;
    }
    snprintf(status, sizeof status, "%s", expire_downloads ?
             "Expiração ativa: novos NSP e XCI serão removidos após 8 horas." : "Expiração desativada.");
}

static bool read_item(Item *item, const cJSON *object)
{
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(object, "size");
    if (!cJSON_IsNumber(size) || !(size->valuedouble >= 1 && size->valuedouble <= FILE_LIMIT))
        return false;
    item->size = (curl_off_t)size->valuedouble;
    if ((double)item->size != size->valuedouble ||
        !copy_field(item->title, sizeof item->title, object, "title", true) ||
        !copy_field(item->game, sizeof item->game, object, "game", false) ||
        !copy_field(item->requirement, sizeof item->requirement, object, "game_requirement", false) ||
        !copy_field(item->file, sizeof item->file, object, "file", true) ||
        !copy_field(item->url, sizeof item->url, object, "url", true) ||
        !copy_field(item->hash, sizeof item->hash, object, "sha256", true) ||
        !https_url(item->url) || !safe_file(item->file) || strlen(item->hash) != 64)
        return false;
    for (size_t i = 0; i < 64; ++i) {
        if (!isxdigit((unsigned char)item->hash[i])) return false;
        item->hash[i] = (char)tolower((unsigned char)item->hash[i]);
    }
    return true;
}

static size_t write_data(char *data, size_t size, size_t count_bytes, void *opaque)
{
    Transfer *transfer = opaque;
    if (!size || count_bytes > (uint64_t)transfer->remaining / size) return 0;
    size_t written = fwrite(data, 1, size * count_bytes, transfer->file);
    transfer->remaining -= written;
    return written;
}

static int progress(void *opaque, curl_off_t total, curl_off_t now,
                    curl_off_t upload_total, curl_off_t upload_now)
{
    (void)total;
    (void)upload_total;
    (void)upload_now;
    Transfer *transfer = opaque;
    padUpdate(&pad);
    if (!appletMainLoop() || (padGetButtonsDown(&pad) & HidNpadButton_B)) {
        transfer->stopped = true;
        return 1;
    }
    if (transfer->total) {
        int percent = (int)((transfer->offset + now) * 100 / transfer->total);
        if (percent != transfer->last_percent && percent <= 100) {
            draw_task("Baixando arquivo", "B  Cancelar e guardar o parcial", percent);
            transfer->last_percent = percent;
        }
    }
    return 0;
}

static CURLcode transfer_file(const char *url, Transfer *transfer)
{
    if (R_FAILED(socketInitializeDefault())) return CURLE_COULDNT_CONNECT;
    CURL *curl = curl_easy_init();
    CURLcode result = CURLE_FAILED_INIT;
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
        curl_easy_setopt(curl, CURLOPT_CAINFO, DATA "/cacert.pem");
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 25L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, transfer->total ? 1800L : 35L);
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, transfer->offset);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, transfer);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, transfer);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Xfoil/" APP_VERSION);
        result = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    socketExit();
    return result;
}

static void load_catalog(const char *source)
{
    const char *path = DATA "/catalog.tmp";
    FILE *file = fopen(path, "w+b");
    if (!file) {
        snprintf(status, sizeof status, "Nao foi possivel abrir o arquivo temporario.");
        return;
    }
    Transfer transfer = {.file = file, .remaining = CATALOG_LIMIT};
    CURLcode result = transfer_file(source, &transfer);
    size_t length = CATALOG_LIMIT - transfer.remaining;
    char *json = result == CURLE_OK ? malloc(length + 1) : NULL;
    Item *replacement = NULL;
    size_t valid_count = 0;
    bool valid_catalog = false;
    if (json && !fseek(file, 0, SEEK_SET) && fread(json, 1, length, file) == length) {
        json[length] = 0;
        cJSON *root = cJSON_ParseWithLengthOpts(json, length + 1, NULL, true);
        const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "files");
        const cJSON *entry;
        if (cJSON_IsArray(list)) {
            size_t capacity = (size_t)cJSON_GetArraySize(list);
            if (capacity > MAX_ITEMS) capacity = MAX_ITEMS;
            if (capacity) replacement = malloc(capacity * sizeof *replacement);
            valid_catalog = !capacity || replacement;
            if (replacement) {
                cJSON_ArrayForEach(entry, list) {
                    if (valid_count == capacity) break;
                    if (read_item(&replacement[valid_count], entry)) ++valid_count;
                }
                valid_catalog = valid_count > 0;
            }
        }
        cJSON_Delete(root);
    }
    free(json);
    fclose(file);
    remove(path);
    if (valid_catalog) {
        free(items);
        items = replacement;
        count = valid_count;
        snprintf(status, sizeof status, "%zu arquivos com tamanho e SHA-256 validos.", count);
    } else if (result != CURLE_OK) {
        snprintf(status, sizeof status, "Catalogo: %s.", curl_easy_strerror(result));
    } else {
        free(replacement);
        snprintf(status, sizeof status, "Catalogo invalido ou memoria insuficiente.");
    }
}

static bool verify_file(const char *path, const Item *item)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    Sha256Context context;
    unsigned char digest[32], buffer[8192];
    char hex[65];
    sha256ContextCreate(&context);
    curl_off_t total = 0;
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof buffer, file)) != 0) {
        sha256ContextUpdate(&context, buffer, bytes);
        total += bytes;
    }
    bool valid = !ferror(file) && total == item->size;
    fclose(file);
    sha256ContextGetHash(&context, digest);
    for (size_t i = 0; i < sizeof digest; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return valid && !strcmp(hex, item->hash);
}

static bool download(const Item *item)
{
    char final[256], partial[256];
    snprintf(final, sizeof final, DOWNLOADS "/%s", item->file);
    snprintf(partial, sizeof partial, DOWNLOADS "/%s.part", item->hash);
    struct stat info;
    if (!stat(final, &info)) {
        draw_task("Verificando arquivo", item->file, -1);
        bool valid = verify_file(final, item);
        snprintf(status, sizeof status, "%s", valid ?
                 "O arquivo ja foi baixado e conferido." : "Ja existe outro arquivo com este nome.");
        return valid;
    }
    curl_off_t offset = stat(partial, &info) == 0 ? info.st_size : 0;
    if (offset > item->size) {
        if (remove(partial)) {
            snprintf(status, sizeof status, "Nao foi possivel limpar o parcial invalido.");
            return false;
        }
        offset = 0;
    }
    if (offset < item->size) {
        FILE *file = fopen(partial, offset ? "ab" : "wb");
        if (!file) {
            snprintf(status, sizeof status, "Nao foi possivel gravar no microSD.");
            return false;
        }
        Transfer transfer = {.file = file, .remaining = item->size - offset,
                             .offset = offset, .total = item->size, .last_percent = -1};
        CURLcode result = transfer_file(item->url, &transfer);
        if (fclose(file) && result == CURLE_OK) result = CURLE_WRITE_ERROR;
        if (result != CURLE_OK || transfer.remaining) {
            if (result == CURLE_RANGE_ERROR) {
                remove(partial);
                snprintf(status, sizeof status, "Servidor sem retomada. Pressione A para reiniciar.");
            } else {
                snprintf(status, sizeof status, "Download incompleto: %s. Pressione A para retomar.",
                         transfer.stopped ? "cancelado" :
                         result == CURLE_OK ? "tamanho curto" : curl_easy_strerror(result));
            }
            return false;
        }
    }
    draw_task("Conferindo SHA-256", item->file, -1);
    if (!verify_file(partial, item)) {
        remove(partial);
        snprintf(status, sizeof status, "SHA-256 ou tamanho divergente. Arquivo descartado.");
    } else if (rename(partial, final)) {
        snprintf(status, sizeof status, "Arquivo conferido. Falha ao renomear o parcial.");
    } else {
        mark_for_expiry(item, final);
        snprintf(status, sizeof status, "Arquivo baixado e verificado em downloads/.");
        return true;
    }
    return false;
}

static bool open_dbi(const Item *item)
{
    char path[256];
    snprintf(path, sizeof path, DOWNLOADS "/%s", item->file);
    FILE *file = fopen(path, "rb");
    char magic[4];
    bool xci = !strcasecmp(strrchr(item->file, '.'), ".xci");
    bool valid = file && (!xci || !fseek(file, 0x100, SEEK_SET)) &&
                 fread(magic, 1, sizeof magic, file) == sizeof magic &&
                 !memcmp(magic, xci ? "HEAD" : "PFS0", sizeof magic);
    if (file) fclose(file);
    if (!valid) {
        snprintf(status, sizeof status, "O arquivo baixado nao tem o formato NSP ou XCI esperado.");
        return false;
    }
    if (!envHasNextLoad()) {
        snprintf(status, sizeof status, "Abra o DBI pelo Homebrew Menu. Arquivo em switch/xfoil/downloads.");
        return false;
    }
    static const char *const locations[] = {
        "sdmc:/switch/DBI/DBI.nro", "sdmc:/switch/dbi.nro"
    };
    for (size_t i = 0; i < sizeof locations / sizeof *locations; ++i) {
        file = fopen(locations[i], "rb");
        if (!file) continue;
        valid = !fseek(file, 0x10, SEEK_SET) &&
                fread(magic, 1, sizeof magic, file) == sizeof magic &&
                !memcmp(magic, "NRO0", sizeof magic);
        fclose(file);
        if (valid && R_SUCCEEDED(envSetNextLoad(locations[i], locations[i]))) return true;
    }
    snprintf(status, sizeof status, "DBI indisponivel. Arquivo salvo em switch/xfoil/downloads.");
    return false;
}

static bool keyboard(char *text, size_t size, const char *prompt)
{
    SwkbdConfig keyboard_config;
    if (R_FAILED(swkbdCreate(&keyboard_config, 0))) return false;
    swkbdConfigMakePresetDefault(&keyboard_config);
    swkbdConfigSetGuideText(&keyboard_config, prompt);
    swkbdConfigSetStringLenMax(&keyboard_config, (u32)(size - 1));
    swkbdConfigSetInitialText(&keyboard_config, text);
    Result result = swkbdShow(&keyboard_config, text, size);
    swkbdClose(&keyboard_config);
    return R_SUCCEEDED(result);
}

static bool contains(const char *text, const char *query)
{
    if (!*query) return true;
    for (; *text; ++text) {
        size_t i = 0;
        while (query[i] && text[i] &&
               tolower((unsigned char)query[i]) == tolower((unsigned char)text[i])) ++i;
        if (!query[i]) return true;
    }
    return false;
}

int main(void)
{
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    Result screen = framebufferCreate(&display, nwindowGetDefault(), UI_WIDTH, UI_HEIGHT,
                                      PIXEL_FORMAT_RGBA_8888, 2);
    if (R_SUCCEEDED(screen)) {
        screen = framebufferMakeLinear(&display);
        if (R_FAILED(screen)) framebufferClose(&display);
    }
    if (R_FAILED(screen)) return 1;
    mkdir(DATA, 0777);
    mkdir(DOWNLOADS, 0777);
    mkdir(RETENTION, 0777);
    struct stat expiry_setting;
    expire_downloads = !stat(EXPIRY_SETTING, &expiry_setting) && S_ISREG(expiry_setting.st_mode);
    unsigned expired = clear_expired();
    if (expired) snprintf(status, sizeof status, "%u downloads expirados removidos.", expired);
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        draw_task("Falha ao inicializar HTTPS", "+  Sair", -1);
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            svcSleepThread(16000000);
        }
        framebufferClose(&display);
        return 1;
    }
    for (size_t i = 0; i < 4; ++i) {
        char path[80];
        snprintf(path, sizeof path, DATA "/servidor-%zu.txt", i + 1);
        FILE *config = fopen(path, "r");
        if (!config && errno == ENOENT) {
            snprintf(path, sizeof path, "sdmc:/switch/indicax-servers/servidor-%zu.txt", i + 1);
            config = fopen(path, "r");
        }
        if (config) {
            if (fgets(sources[i], URL_CAP, config)) sources[i][strcspn(sources[i], "\r\n")] = 0;
            fclose(config);
        }
    }
    size_t selected = 0, server = 0, matches = 0, visible[MAX_ITEMS];
    bool confirm = false, ready = false, redraw = true, refresh_matches = true;
    while (appletMainLoop()) {
        if (refresh_matches) {
            matches = 0;
            for (size_t i = 0; i < count; ++i)
                if (contains(items[i].title, filter) || contains(items[i].game, filter))
                    visible[matches++] = i;
            if (selected >= matches) selected = 0;
            refresh_matches = false;
        }
        if (redraw) {
            draw_hud(server, selected, visible, matches, confirm, ready);
            redraw = false;
        }
        padUpdate(&pad);
        u64 buttons = padGetButtonsDown(&pad);
        if (buttons & HidNpadButton_Plus) break;
        if ((buttons & HidNpadButton_B) && confirm) {
            confirm = false;
            ready = false;
            redraw = true;
            continue;
        }
        if (confirm && matches && (buttons & HidNpadButton_A)) {
            const Item *item = &items[visible[selected]];
            if (ready) {
                if (open_dbi(item)) break;
            } else {
                ready = download(item) && title_package(item->file);
            }
        } else if (!confirm) {
            if (buttons & HidNpadButton_ZR) toggle_expiry();
            if (buttons & HidNpadButton_R) {
                server = (server + 1) % 4;
                free(items);
                items = NULL;
                count = 0;
                refresh_matches = true;
                filter[0] = 0;
                snprintf(status, sizeof status, "Pressione Y para carregar este servidor.");
            }
            if ((buttons & HidNpadButton_AnyDown) && selected + 1 < matches) ++selected;
            if ((buttons & HidNpadButton_AnyUp) && selected) --selected;
            if ((buttons & HidNpadButton_A) && matches && count) {
                confirm = true;
                status[0] = 0;
            }
            if ((buttons & HidNpadButton_L) && keyboard(filter, sizeof filter, "Buscar jogo ou arquivo"))
                refresh_matches = true;
            if (buttons & HidNpadButton_X) {
                char address[URL_CAP];
                memcpy(address, sources[server], sizeof address);
                if (keyboard(address, sizeof address, "URL HTTPS do catalogo JSON")) {
                    if (!https_url(address)) {
                        snprintf(status, sizeof status, "URL HTTPS invalida.");
                    } else {
                        memcpy(sources[server], address, sizeof address);
                        free(items);
                        items = NULL;
                        count = 0;
                        refresh_matches = true;
                        char path[80];
                        snprintf(path, sizeof path, DATA "/servidor-%zu.txt", server + 1);
                        FILE *config = fopen(path, "w");
                        bool saved = false;
                        if (config) {
                            saved = fprintf(config, "%s\n", address) > 0;
                            if (fclose(config)) saved = false;
                        }
                        snprintf(status, sizeof status, "%s", saved ?
                                 "Pressione Y para carregar o catalogo." :
                                 "Servidor ajustado nesta sessao. O microSD nao salvou a URL.");
                    }
                }
            }
            if (buttons & HidNpadButton_Y) {
                if (https_url(sources[server])) {
                    draw_task("Carregando catálogo", "B  Cancelar", -1);
                    load_catalog(sources[server]);
                    refresh_matches = true;
                } else snprintf(status, sizeof status, "Informe primeiro um servidor HTTPS com X.");
            }
        }
        redraw |= buttons != 0;
        svcSleepThread(16000000);
    }
    free(items);
    curl_global_cleanup();
    framebufferClose(&display);
    return 0;
}
