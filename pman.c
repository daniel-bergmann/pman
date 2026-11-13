/*
 * pman - a tiny "man" for PHP functions.
 *
 * Fetches the online PHP manual (https://www.php.net/<function>) and prints
 * the name, version info, purpose, signature, description, parameters,
 * return values, code examples and "see also" section for the given
 * function.
 *
 * When stdout is a terminal the output is shown in a small built-in pager:
 *   j / Down / Space   scroll down
 *   k / Up / PageUp    scroll up
 *   q / :q / Ctrl-C    quit
 *
 * The HTTP(S) request is delegated to the system `curl` binary, which is
 * always available and handles TLS, redirects and compression. Everything
 * else (URL construction, fetching, HTML parsing and rendering) is done here.
 *
 * Build:  cc -O2 -Wall -Wextra -o pman pman.c
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static const char *PHP_MANUAL = "https://www.php.net/";

/* ------------------------------------------------------------------ */
/* Output buffer                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_init(StrBuf *sb) {
    sb->cap = 8192;
    sb->data = malloc(sb->cap);
    if (!sb->data) {
        sb->cap = 0;
        sb->len = 0;
        return;
    }
    sb->data[0] = '\0';
    sb->len = 0;
}

static void sb_free(StrBuf *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static int sb_reserve(StrBuf *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap)
        return 0;
    size_t ncap = sb->cap ? sb->cap * 2 : 8192;
    while (ncap < sb->len + extra + 1)
        ncap *= 2;
    char *nd = realloc(sb->data, ncap);
    if (!nd)
        return -1;
    sb->data = nd;
    sb->cap = ncap;
    return 0;
}

static void sb_append(StrBuf *sb, const char *s, size_t n) {
    if (sb_reserve(sb, n) != 0)
        return;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_printf(StrBuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    if (sb_reserve(sb, (size_t)n) != 0) {
        va_end(ap2);
        return;
    }
    vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)n;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static char *extract_between(const char *hay, const char *start, const char *end) {
    const char *s = strstr(hay, start);
    if (!s)
        return NULL;
    s += strlen(start);
    const char *e = strstr(s, end);
    if (!e)
        return NULL;

    size_t len = (size_t)(e - s);
    char *out = malloc(len + 1);
    if (!out)
        return NULL;
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

/* Like extract_between, but neither the start nor the end marker may lie at
 * or beyond `limit` (pass NULL for no bound). */
static char *extract_between_bounded(const char *hay, const char *start,
                                     const char *end, const char *limit) {
    const char *s = strstr(hay, start);
    if (!s || (limit && s >= limit))
        return NULL;
    s += strlen(start);
    const char *e = strstr(s, end);
    if (!e || (limit && e >= limit))
        return NULL;

    size_t len = (size_t)(e - s);
    char *out = malloc(len + 1);
    if (!out)
        return NULL;
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static int utf8_encode(unsigned long cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Decode an HTML entity name (without the leading '&' and trailing ';').
 * Returns the number of bytes written to out, or 0 for unknown entities. */
static int decode_entity(const char *name, size_t namelen, char *out) {
    static const struct {
        const char *n;
        const char *u;
    } map[] = {
        {"lt", "<"},       {"gt", ">"},       {"amp", "&"},
        {"quot", "\""},    {"apos", "'"},     {"nbsp", " "},
        {"mdash", "\xE2\x80\x94"},  {"ndash", "\xE2\x80\x93"},
        {"hellip", "\xE2\x80\xA6"}, {"bull", "\xE2\x80\xA2"},
        {"rarr", "\xE2\x86\x92"},   {"larr", "\xE2\x86\x90"},
        {"harr", "\xE2\x86\x94"},   {"raquo", "\xC2\xBB"},
        {"laquo", "\xC2\xAB"},      {"middot", "\xC2\xB7"},
        {"copy", "\xC2\xA9"},       {"reg", "\xC2\xAE"},
        {"deg", "\xC2\xB0"},        {"plusmn", "\xC2\xB1"},
        {"times", "\xC3\x97"},      {"divide", "\xC3\xB7"},
        {"eacute", "\xC3\xA9"},     {"agrave", "\xC3\xA0"},
        {"uuml", "\xC3\xBC"},       {"ouml", "\xC3\xB6"},
        {"szlig", "\xC3\x9F"},      {"ccedil", "\xC3\xA7"},
    };

    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strlen(map[i].n) == namelen && strncmp(map[i].n, name, namelen) == 0) {
            strcpy(out, map[i].u);
            return (int)strlen(map[i].u);
        }
    }

    if (namelen > 1 && name[0] == '#') {
        size_t i = 1;
        int base = 10;
        if (name[1] == 'x' || name[1] == 'X') {
            base = 16;
            i = 2;
        }
        unsigned long cp = 0;
        for (; i < namelen; i++) {
            char c = name[i];
            int d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (base == 16 && c >= 'a' && c <= 'f')
                d = c - 'a' + 10;
            else if (base == 16 && c >= 'A' && c <= 'F')
                d = c - 'A' + 10;
            else
                return 0;
            if (cp > (0x10FFFFUL - (unsigned long)d) / (unsigned long)base)
                return 0;
            cp = cp * (unsigned long)base + (unsigned long)d;
        }
        return utf8_encode(cp, out);
    }

    return 0;
}

/* Strip HTML tags, decode common entities, collapse whitespace, trim. */
static char *strip_html(const char *src) {
    size_t cap = strlen(src) + 1;
    char *out = malloc(cap);
    if (!out)
        return NULL;

    size_t o = 0;
    int in_tag = 0;
    int pending_space = 0;
    const char *p = src;

    while (*p && o + 1 < cap) {
        if (in_tag) {
            if (*p == '>')
                in_tag = 0;
            p++;
            continue;
        }
        if (*p == '<') {
            in_tag = 1;
            p++;
            continue;
        }
        if (*p == '&') {
            const char *semi = strchr(p + 1, ';');
            if (semi && (size_t)(semi - (p + 1)) < 32) {
                size_t nlen = (size_t)(semi - (p + 1));
                char tmp[8];
                int n = decode_entity(p + 1, nlen, tmp);
                if (n > 0) {
                    for (int i = 0; i < n; i++) {
                        if (o + 1 < cap)
                            out[o++] = tmp[i];
                    }
                    p = semi + 1;
                    pending_space = 0;
                    continue;
                }
                /* Unknown entity: drop it entirely. */
                p = semi + 1;
                continue;
            }
            out[o++] = '&';
            pending_space = 0;
            p++;
            continue;
        }
        if (isspace((unsigned char)*p)) {
            pending_space = 1;
            p++;
            continue;
        }
        if (pending_space) {
            out[o++] = ' ';
            pending_space = 0;
        }
        out[o++] = *p;
        p++;
    }
    out[o] = '\0';

    while (o > 0 && out[o - 1] == ' ')
        out[--o] = '\0';
    while (*out == ' ')
        memmove(out, out + 1, strlen(out));

    return out;
}

/* Strip HTML tags and decode entities, but preserve whitespace exactly
 * (used for code examples, where newlines and indentation matter). */
static char *strip_code(const char *src) {
    size_t cap = strlen(src) + 1;
    char *out = malloc(cap);
    if (!out)
        return NULL;

    size_t o = 0;
    int in_tag = 0;
    const char *p = src;

    while (*p && o + 1 < cap) {
        if (in_tag) {
            if (*p == '>')
                in_tag = 0;
            p++;
            continue;
        }
        if (*p == '<') {
            in_tag = 1;
            p++;
            continue;
        }
        if (*p == '&') {
            const char *semi = strchr(p + 1, ';');
            if (semi && (size_t)(semi - (p + 1)) < 32) {
                size_t nlen = (size_t)(semi - (p + 1));
                char tmp[8];
                int n = decode_entity(p + 1, nlen, tmp);
                if (n > 0) {
                    for (int i = 0; i < n; i++) {
                        if (o + 1 < cap)
                            out[o++] = tmp[i];
                    }
                    p = semi + 1;
                    continue;
                }
                /* Unknown entity: drop it entirely. */
                p = semi + 1;
                continue;
            }
            out[o++] = '&';
            p++;
            continue;
        }
        out[o++] = *p;
        p++;
    }
    out[o] = '\0';

    while (o > 0 &&
           (out[o - 1] == ' ' || out[o - 1] == '\t' ||
            out[o - 1] == '\n' || out[o - 1] == '\r'))
        out[--o] = '\0';
    while (*out == ' ' || *out == '\t' || *out == '\n' || *out == '\r')
        memmove(out, out + 1, strlen(out));

    return out;
}

static int read_all(int fd, char **out, size_t *outlen) {
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    if (!buf)
        return -1;

    for (;;) {
        if (len + 4096 > cap) {
            size_t ncap = cap * 2;
            char *nb = realloc(buf, ncap);
            if (!nb) {
                free(buf);
                return -1;
            }
            buf = nb;
            cap = ncap;
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            return -1;
        }
        if (r == 0)
            break;
        len += (size_t)r;
    }
    buf[len] = '\0';
    *out = buf;
    *outlen = len;
    return 0;
}

/* Fetch a URL by running `curl -sS --compressed -L --max-time 20 <url>`.
 * Returns curl's exit status, or -1 on local errors. */
static int fetch_url(const char *url, char **out, size_t *outlen) {
    int fds[2];
    if (pipe(fds) != 0) {
        perror("pman: pipe");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("pman: fork");
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execlp("curl", "curl", "-sS", "--compressed", "-L",
               "--max-time", "20", url, (char *)NULL);
        _exit(127);
    }

    close(fds[1]);
    if (read_all(fds[0], out, outlen) != 0) {
        close(fds[0]);
        waitpid(pid, NULL, 0);
        return -1;
    }
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                          */
/* ------------------------------------------------------------------ */

static void print_section_if(StrBuf *out, const char *b, const char *r,
                             const char *title, const char *prefix,
                             const char *text) {
    const char *p = text;
    if (prefix) {
        size_t plen = strlen(prefix);
        if (strncmp(p, prefix, plen) == 0)
            p += plen;
    }
    while (*p == ' ')
        p++;
    if (*p)
        sb_printf(out, "\n%s%s%s\n  %s\n", b, title, r, p);
}

/* Print a multi-line block, indenting each non-empty line. */
static void print_indented(StrBuf *out, const char *text, const char *indent) {
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0) {
            sb_append(out, indent, strlen(indent));
            sb_append(out, p, len);
        }
        sb_append(out, "\n", 1);
        if (!nl)
            break;
        p = nl + 1;
    }
}

static void print_entry(StrBuf *out, char *html) {
    int tty = isatty(STDOUT_FILENO);
    const char *b = tty ? "\033[1m" : "";
    const char *d = tty ? "\033[2m" : "";
    const char *r = tty ? "\033[0m" : "";

    char *raw = extract_between(html, "<h1 class=\"refname\">", "</h1>");
    char *name = raw ? strip_html(raw) : NULL;
    free(raw);

    raw = extract_between(html, "<p class=\"verinfo\">", "</p>");
    char *ver = raw ? strip_html(raw) : NULL;
    free(raw);

    raw = extract_between(html, "<span class=\"dc-title\">", "</span>");
    char *title = raw ? strip_html(raw) : NULL;
    free(raw);

    sb_printf(out, "%s%s%s\n", b, name ? name : "?", r);
    if (ver || title) {
        sb_printf(out, "%s", d);
        if (ver)
            sb_printf(out, "%s", ver);
        if (ver && title)
            sb_printf(out, " \342\200\224 "); /* em dash */
        if (title)
            sb_printf(out, "%s", title);
        sb_printf(out, "%s\n", r);
    }

    raw = extract_between(html, "<div class=\"methodsynopsis dc-description\">",
                          "</div>");
    char *syn = raw ? strip_html(raw) : NULL;
    free(raw);
    if (syn && syn[0]) {
        const char *p = syn;
        if (strncmp(p, "function ", 9) == 0)
            p += 9;
        if (*p)
            sb_printf(out, "\n%sSignature%s\n  %s\n", b, r, p);
    }
    free(syn);

    raw = extract_between(html, "<p class=\"para rdfs-comment\">", "</p>");
    if (!raw) {
        raw = extract_between(html, "<p class=\"simpara\">", "</p>");
    }
    char *desc = raw ? strip_html(raw) : NULL;
    free(raw);
    if (desc && desc[0])
        print_section_if(out, b, r, "Description", NULL, desc);
    free(desc);

    /* Parameters */
    const char *psec = strstr(html, "<div class=\"refsect1 parameters");
    if (psec) {
        const char *pend = strstr(psec + 1, "<div class=\"refsect1");
        if (!pend)
            pend = psec + strlen(psec);

        int first = 1;
        const char *p = psec;
        while (p < pend) {
            const char *dt = strstr(p, "<dt><code class=\"parameter\">");
            if (!dt || dt >= pend)
                break;
            const char *ns = dt + strlen("<dt><code class=\"parameter\">");
            const char *ne = strstr(ns, "</code></dt>");
            if (!ne || ne >= pend)
                break;

            char *pname = strndup(ns, (size_t)(ne - ns));
            const char *ds = ne + strlen("</code></dt>");
            const char *dend = strstr(ds, "<dt><code class=\"parameter\">");
            if (!dend || dend >= pend)
                dend = pend;

            char *rawd = strndup(ds, (size_t)(dend - ds));
            char *pdesc = strip_html(rawd);

            if (first) {
                sb_printf(out, "\n%sParameters%s\n", b, r);
                first = 0;
            }
            sb_printf(out, "  %s%s%s\n    %s\n", b, pname, r,
                      pdesc ? pdesc : "");

            free(pname);
            free(rawd);
            free(pdesc);
            p = dt + 1;
        }
    }

    /* Return values */
    const char *rsec = strstr(html, "<div class=\"refsect1 returnvalues");
    if (rsec) {
        const char *rend = strstr(rsec + 1, "<div class=\"refsect1");
        if (!rend)
            rend = rsec + strlen(rsec);
        raw = strndup(rsec, (size_t)(rend - rsec));
        char *rtext = raw ? strip_html(raw) : NULL;
        free(raw);
        if (rtext)
            print_section_if(out, b, r, "Return Values", "Return Values",
                             rtext);
        free(rtext);
    }

    /* Examples */
    const char *esec = strstr(html, "<div class=\"refsect1 examples");
    if (esec) {
        const char *eend = strstr(esec + 1, "<div class=\"refsect1");
        if (!eend)
            eend = esec + strlen(esec);

        int first_example = 1;
        const char *p = esec;
        while (p < eend) {
            const char *ex = strstr(p, "<div class=\"example\"");
            if (!ex || ex >= eend)
                break;
            const char *nex = strstr(ex + 1, "<div class=\"example\"");
            const char *ex_end = (nex && nex < eend) ? nex : eend;

            char *rawtitle =
                extract_between_bounded(ex, "<p><strong>", "</p>", ex_end);
            char *extitle = rawtitle ? strip_html(rawtitle) : NULL;
            free(rawtitle);

            /* First PHP code block in this example. */
            char *code = NULL;
            const char *co = strstr(ex, "<pre><code");
            if (co && co < ex_end) {
                const char *gt = strchr(co, '>');
                if (gt && gt < ex_end) {
                    const char *ce = strstr(gt, "</code></pre>");
                    if (ce && ce < ex_end) {
                        raw = strndup(gt + 1, (size_t)(ce - (gt + 1)));
                        code = raw ? strip_code(raw) : NULL;
                        free(raw);
                    }
                }
            }

            /* Optional program output block. */
            char *output = NULL;
            const char *screen =
                strstr(ex, "<div class=\"example-contents screen\"");
            if (screen && screen < ex_end) {
                const char *pre = strstr(screen, "<pre");
                if (pre && pre < ex_end) {
                    const char *gt = strchr(pre, '>');
                    if (gt && gt < ex_end) {
                        const char *pe = strstr(gt, "</pre>");
                        if (pe && pe < ex_end) {
                            raw = strndup(gt + 1, (size_t)(pe - (gt + 1)));
                            output = raw ? strip_code(raw) : NULL;
                            free(raw);
                        }
                    }
                }
            }

            if (extitle || code || output) {
                if (first_example) {
                    sb_printf(out, "\n%sExamples%s\n", b, r);
                    first_example = 0;
                }
                if (extitle && extitle[0])
                    sb_printf(out, "\n  %s%s%s\n", b, extitle, r);
                if (code && code[0])
                    print_indented(out, code, "    ");
                if (output && output[0]) {
                    sb_printf(out, "\n  %sOutput%s\n", d, r);
                    print_indented(out, output, "    ");
                }
            }

            free(extitle);
            free(code);
            free(output);
            p = ex + 1;
        }
    }

    /* See also */
    const char *ssec = strstr(html, "<div class=\"refsect1 seealso");
    if (ssec) {
        const char *send = strstr(ssec, "</div>");
        if (!send)
            send = ssec + strlen(ssec);
        int first = 1;
        const char *p = ssec;
        while (p < send) {
            const char *li = strstr(p, "<li>");
            if (!li || li >= send)
                break;
            const char *le = strstr(li, "</li>");
            if (!le || le >= send)
                break;
            raw = strndup(li + 4, (size_t)(le - (li + 4)));
            char *ltext = raw ? strip_html(raw) : NULL;
            free(raw);
            if (ltext && ltext[0]) {
                if (first) {
                    sb_printf(out, "\n%sSee Also%s\n", b, r);
                    first = 0;
                }
                sb_printf(out, "  %s\n", ltext);
            }
            free(ltext);
            p = le + 1;
        }
    }

    free(name);
    free(ver);
    free(title);
}

/* ------------------------------------------------------------------ */
/* Pager                                                              */
/* ------------------------------------------------------------------ */

enum {
    KEY_UP = 256,
    KEY_DOWN,
    KEY_PGUP,
    KEY_PGDOWN,
};

typedef struct {
    const char *s;
    size_t len;
} DispLine;

static DispLine *disp_lines;
static size_t disp_count;
static size_t disp_cap;

static void add_disp(const char *s, size_t len) {
    if (disp_count == disp_cap) {
        size_t nc = disp_cap ? disp_cap * 2 : 256;
        DispLine *nl = realloc(disp_lines, nc * sizeof(DispLine));
        if (!nl)
            return;
        disp_lines = nl;
        disp_cap = nc;
    }
    disp_lines[disp_count].s = s;
    disp_lines[disp_count].len = len;
    disp_count++;
}

static void build_display_lines(const char *text, size_t width) {
    disp_count = 0;
    if (width < 8)
        width = 8;

    const char *p = text;
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0) {
            size_t off = 0;
            while (len - off > width) {
                add_disp(p + off, width);
                off += width;
            }
            add_disp(p + off, len - off);
        } else {
            add_disp(p, 0);
        }
        if (!nl)
            break;
        p = nl + 1;
        if (*p == '\0')
            break;
    }
}

static int read_key(int ttyfd) {
    unsigned char c;
    ssize_t n = read(ttyfd, &c, 1);
    if (n <= 0)
        return -1;

    if (c == 0x03 || c == 0x04) /* Ctrl-C / Ctrl-D */
        return 'q';
    if (c != 0x1b)
        return c;

    /* Escape sequence (arrow keys / page keys). */
    struct pollfd pfd;
    pfd.fd = ttyfd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 30) <= 0)
        return 0;

    unsigned char b;
    if (read(ttyfd, &b, 1) != 1 || b != '[')
        return 0;
    if (read(ttyfd, &b, 1) != 1)
        return 0;
    if (b == 'A')
        return KEY_UP;
    if (b == 'B')
        return KEY_DOWN;
    if (b == '5' || b == '6') {
        unsigned char tilde;
        if (read(ttyfd, &tilde, 1) == 1 && tilde == '~')
            return b == '5' ? KEY_PGUP : KEY_PGDOWN;
    }
    return 0;
}

static void pager_redraw(long top, int rows, int cols, int colon) {
    fputs("\x1b[H\x1b[2J", stdout);

    int visible = rows - 1;
    if (visible < 1)
        visible = 1;
    size_t end = (size_t)top + (size_t)visible;
    if (end > disp_count)
        end = disp_count;

    for (size_t i = (size_t)top; i < end; i++) {
        if (disp_lines[i].len)
            fwrite(disp_lines[i].s, 1, disp_lines[i].len, stdout);
        fputc('\n', stdout);
    }

    char status[512];
    int r;
    if (colon)
        r = snprintf(status, sizeof(status),
                     " pman  %ld-%zu/%zu  j/k: scroll  q: quit  :",
                     top + 1, end, disp_count);
    else
        r = snprintf(status, sizeof(status),
                     " pman  %ld-%zu/%zu  j/k: scroll  q: quit ",
                     top + 1, end, disp_count);
    if (r < 0)
        status[0] = '\0';

    printf("\x1b[%d;1H", rows);
    printf("\x1b[7m%-*s\x1b[0m", cols, status);
    fflush(stdout);
}

static void run_pager(const char *text) {
    if (!text || !*text)
        return;

    int ttyfd = open("/dev/tty", O_RDONLY | O_CLOEXEC);
    if (ttyfd < 0) {
        fwrite(text, 1, strlen(text), stdout);
        return;
    }

    struct winsize ws;
    ws.ws_row = 0;
    ws.ws_col = 0;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) {
        ws.ws_row = 0;
        ws.ws_col = 0;
    }
    int rows = ws.ws_row > 0 ? ws.ws_row : 24;
    int cols = ws.ws_col > 0 ? ws.ws_col : 80;
    if (cols < 8)
        cols = 80;

    struct termios old, raw;
    if (tcgetattr(ttyfd, &old) != 0) {
        close(ttyfd);
        fwrite(text, 1, strlen(text), stdout);
        return;
    }
    raw = old;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(ttyfd, TCSANOW, &raw) != 0) {
        close(ttyfd);
        fwrite(text, 1, strlen(text), stdout);
        return;
    }

    fputs("\x1b[?1049h\x1b[H\x1b[2J", stdout);
    fflush(stdout);

    build_display_lines(text, (size_t)(cols - 1 > 0 ? cols - 1 : 1));

    long top = 0;
    int colon = 0;
    int visible = rows - 1;
    if (visible < 1)
        visible = 1;

    for (;;) {
        pager_redraw(top, rows, cols, colon);
        int key = read_key(ttyfd);
        if (key == -1)
            break;
        if (key == 0)
            continue;

        if (colon) {
            if (key == 'q')
                break;
            colon = 0;
            continue;
        }

        if (key == ':') {
            colon = 1;
        } else if (key == 'q') {
            break;
        } else if (key == 'j' || key == KEY_DOWN) {
            if ((size_t)top + (size_t)visible < disp_count)
                top++;
        } else if (key == 'k' || key == KEY_UP) {
            if (top > 0)
                top--;
        } else if (key == ' ' || key == KEY_PGDOWN) {
            long max_top = disp_count > (size_t)visible
                               ? (long)(disp_count - (size_t)visible)
                               : 0;
            top += visible;
            if (top > max_top)
                top = max_top;
        } else if (key == KEY_PGUP) {
            top -= visible;
            if (top < 0)
                top = 0;
        }
    }

    fputs("\x1b[?1049l", stdout);
    fflush(stdout);
    tcsetattr(ttyfd, TCSANOW, &old);
    close(ttyfd);

    free(disp_lines);
    disp_lines = NULL;
    disp_cap = 0;
    disp_count = 0;
}

/* ------------------------------------------------------------------ */
/* Argument handling                                                  */
/* ------------------------------------------------------------------ */

static int normalize_name(const char *arg, char *buf, size_t bufsz) {
    size_t len = strlen(arg);

    while (len > 0 && (arg[len - 1] == ' ' || arg[len - 1] == '\t'))
        len--;
    if (len >= 2 && arg[len - 2] == '(' && arg[len - 1] == ')')
        len -= 2;
    while (len > 0 && (arg[len - 1] == ' ' || arg[len - 1] == '\t'))
        len--;

    const char *start = arg;
    while (*start == ' ' || *start == '\t')
        start++;
    len -= (size_t)(start - arg);

    if (len == 0 || len >= bufsz)
        return 0;
    for (size_t i = 0; i < len; i++) {
        char c = start[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-'))
            return 0;
    }
    memcpy(buf, start, len);
    buf[len] = '\0';
    return 1;
}

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s <function> [<function> ...]\n", prog);
    fprintf(stderr, "example: %s strlen\n", prog);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    if (argc == 2 &&
        (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage(argv[0]);
        return 0;
    }

    int any_error = 0;
    StrBuf out;
    sb_init(&out);

    for (int i = 1; i < argc; i++) {
        char name[128];
        if (!normalize_name(argv[i], name, sizeof(name))) {
            fprintf(stderr, "pman: invalid function name: %s\n", argv[i]);
            any_error = 1;
            continue;
        }

        size_t url_len = strlen(PHP_MANUAL) + strlen(name) + 1;
        char *url = malloc(url_len);
        if (!url) {
            fprintf(stderr, "pman: out of memory\n");
            return 1;
        }
        snprintf(url, url_len, "%s%s", PHP_MANUAL, name);

        char *html = NULL;
        size_t html_len = 0;
        int rc = fetch_url(url, &html, &html_len);
        free(url);

        if (rc != 0 || !html || html_len == 0) {
            fprintf(stderr, "pman: failed to fetch manual page for %s"
                            " (curl exit %d)\n", name, rc);
            free(html);
            any_error = 1;
            continue;
        }

        if (!strstr(html, "refnamediv")) {
            fprintf(stderr, "pman: no manual entry found for %s\n", name);
            free(html);
            any_error = 1;
            continue;
        }

        if (i > 1)
            sb_printf(&out, "\n");
        print_entry(&out, html);
        free(html);
    }

    if (out.len > 0) {
        if (isatty(STDOUT_FILENO))
            run_pager(out.data);
        else
            fwrite(out.data, 1, out.len, stdout);
    }
    sb_free(&out);

    return any_error ? 1 : 0;
}
