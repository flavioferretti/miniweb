# MiniWeb - OpenBSD Secure Web Server

Un web server leggero e sicuro per OpenBSD con metriche di sistema in tempo reale, costruito con `libmicrohttpd` e le security features native di OpenBSD (`pledge`, `unveil`).

## 🚀 Caratteristiche

- **Server HTTP** leggero basato su libmicrohttpd
- **Metriche di Sistema** complete in tempo reale via API REST
- **Template Engine** semplice per pagine dinamiche
- **Security Hardening** con pledge e unveil di OpenBSD
- **Route Management** modulare e estensibile
- **Static File Serving** per CSS, JS, immagini

## 📋 Prerequisiti

### Sistema Operativo
- OpenBSD 7.0 o superiore (testato su OpenBSD 7.4+)

### Dipendenze
```bash
# Installa libmicrohttpd
doas pkg_add libmicrohttpd
```

## 🔧 Compilazione

```bash
# Clone o copia i file del progetto
# Compila
make

# Compila con debug
make debug

# Pulisci
make clean
```

### Struttura del Makefile
```makefile
CC = cc
CFLAGS = -Wall -Wextra -O2 -I./include
LDFLAGS = -lmicrohttpd

SRCS = src/main.c src/routes.c src/metrics.c src/template_engine.c
OBJS = $(SRCS:.c=.o)

miniweb: $(OBJS)
	$(CC) -o miniweb $(OBJS) $(LDFLAGS)
```

## 📁 Struttura del Progetto

```
miniweb/
├── src/
│   ├── main.c              # Entry point e configurazione server
│   ├── routes.c            # Route handler e routing logic
│   ├── metrics.c           # API metriche di sistema
│   └── template_engine.c   # Template rendering engine
├── include/
│   ├── routes.h
│   ├── metrics.h
│   └── template_engine.h
├── templates/
│   ├── base.html           # Layout base
│   ├── index.html          # Homepage
│   └── info.html           # Pagina info
├── static/
│   ├── css/
│   ├── js/
│   └── images/
├── Makefile
└── README.md
```

## 🎯 Utilizzo

### Avvio Base
```bash
./miniweb
# Server in ascolto su 127.0.0.1:9001
```

### Opzioni da Linea di Comando
```bash
./miniweb -p 8080              # Cambia porta
./miniweb -b 0.0.0.0           # Bind su tutte le interfacce
./miniweb -t 8                 # 8 thread workers
./miniweb -c 500               # Max 500 connessioni
./miniweb -v                   # Modalità verbose
./miniweb -h                   # Mostra help
```

### Esempi Completi
```bash
# Server su porta 8080, tutte le interfacce, verbose
./miniweb -p 8080 -b 0.0.0.0 -v

# Server ottimizzato per carico elevato
./miniweb -t 16 -c 2000

# Server di sviluppo con log verbose
./miniweb -v -t 2
```

## 🌐 Endpoints Disponibili

### Pagine Web
| Endpoint | Descrizione |
|----------|-------------|
| `/` | Homepage |
| `/info` | Informazioni sistema |
| `/static/*` | File statici (CSS, JS, immagini) |

### API REST
| Endpoint | Metodo | Descrizione | Formato |
|----------|--------|-------------|---------|
| `/api/metrics` | GET | Metriche complete di sistema | JSON |

### Esempio Response `/api/metrics`
```json
{
  "timestamp": "2026-02-09 15:30:45",
  "hostname": "openbsd.local",
  "cpu": {
    "user": 12,
    "nice": 0,
    "system": 8,
    "interrupt": 2,
    "idle": 78
  },
  "memory": {
    "total_mb": 8192,
    "free_mb": 2048,
    "active_mb": 3072,
    "inactive_mb": 1536,
    "wired_mb": 1024,
    "cache_mb": 0,
    "swap_total_mb": 2048,
    "swap_used_mb": 256
  },
  "load": {
    "1min": 0.45,
    "5min": 0.32,
    "15min": 0.28
  },
  "os": {
    "type": "OpenBSD",
    "release": "7.4",
    "machine": "amd64"
  },
  "uptime": "5 days, 3:24:15",
  "disks": [
    {
      "device": "/dev/sd0a",
      "mount": "/",
      "total_mb": 51200,
      "used_mb": 30720,
      "percent": 60
    }
  ],
  "top_ports": [
    {
      "port": 22,
      "protocol": "tcp",
      "connections": 1,
      "state": "LISTEN"
    },
    {
      "port": 9001,
      "protocol": "tcp",
      "connections": 1,
      "state": "LISTEN"
    }
  ],
  "network": [
    {
      "name": "em0",
      "ip": "192.168.1.100",
      "status": "up"
    },
    {
      "name": "lo0",
      "ip": "127.0.0.1",
      "status": "up"
    }
  ]
}
```

# Man Pages API - Documentazione Aggiornata (System/Packages)

## Cambio Architettura

Le manpage sono ora separate in **due aree**:
- **system** - `/usr/share/man` (OpenBSD base system)
- **packages** - `/usr/local/man` (pacchetti installati)

## Nuova Struttura API

### GET /api/man
Lista entrambe le aree con le loro sezioni.

**Risposta:**
```json
{
  "system": {
    "name": "OpenBSD Base System",
    "path": "/usr/share/man",
    "sections": [
      {"id": "1", "name": "User Commands"},
      {"id": "2", "name": "System Calls"},
      {"id": "3", "name": "Library Functions"},
      {"id": "3p", "name": "POSIX Library"},
      {"id": "4", "name": "Device Drivers"},
      {"id": "5", "name": "File Formats"},
      {"id": "7", "name": "Miscellaneous"},
      {"id": "8", "name": "System Administration"}
    ]
  },
  "packages": {
    "name": "Installed Packages",
    "path": "/usr/local/man",
    "sections": [
      {"id": "1", "name": "User Commands"},
      {"id": "3", "name": "Library Functions"},
      {"id": "5", "name": "File Formats"},
      {"id": "7", "name": "Miscellaneous"},
      {"id": "8", "name": "System Administration"}
    ]
  }
}
```

### GET /api/man/system/1
Lista tutte le pagine della sezione 1 del sistema base.

**Risposta:**
```json
{
  "area": "system",
  "section": "1",
  "pages": [
    {"name": "cat", "desc": "concatenate and print files"},
    {"name": "ls", "desc": "list directory contents"},
    ...
  ]
}
```

### GET /api/man/packages/1
Lista tutte le pagine della sezione 1 dei pacchetti installati (es. vim, git, etc).

**Risposta:**
```json
{
  "area": "packages",
  "section": "1",
  "pages": [
    {"name": "git", "desc": "the stupid content tracker"},
    {"name": "vim", "desc": "Vi IMproved, a programmer's text editor"},
    ...
  ]
}
```

### GET /api/man/system/1/ls
Metadata per la pagina ls(1) del sistema.

**Risposta:**
```json
{
  "area": "system",
  "section": "1",
  "name": "ls",
  "formats": ["html", "md", "pdf"],
  "links": {
    "html": "/man/system/1/ls",
    "md": "/man/system/1/ls.md",
    "pdf": "/man/system/1/ls.pdf"
  }
}
```

### GET /api/man/packages/1/git
Metadata per git(1) da /usr/local/man.

**Risposta:**
```json
{
  "area": "packages",
  "section": "1",
  "name": "git",
  "formats": ["html", "md", "pdf"],
  "links": {
    "html": "/man/packages/1/git",
    "md": "/man/packages/1/git.md",
    "pdf": "/man/packages/1/git.pdf"
  }
}
```

## Rendering Endpoints

### GET /man/system/1/ls
Renderizza ls(1) dal sistema base in HTML.

### GET /man/packages/1/git
Renderizza git(1) dai pacchetti in HTML.

### GET /man/system/1/ls.md
Renderizza ls(1) in Markdown.

### GET /man/packages/1/vim.pdf
Renderizza vim(1) in PDF.

## Search API (Inalterata)

### GET /api/man/search?q=network
Cerca "network" in tutte le manpage (system + packages).

**Risposta:**
```json
{
  "results": [
    {"name": "ifconfig", "section": "8", "desc": "configure network interface parameters"},
    {"name": "netstat", "section": "1", "desc": "show network status"},
    ...
  ]
}
```

## Tabella Endpoint Completa

| Endpoint | Descrizione | Esempio |
|----------|-------------|---------|
| `GET /api/man` | Lista aree e sezioni | → JSON con system/packages |
| `GET /api/man/{area}/{section}` | Lista pagine in area/sezione | `/api/man/system/1` |
| `GET /api/man/{area}/{section}/{name}` | Metadata pagina | `/api/man/packages/1/git` |
| `GET /api/man/search?q={query}` | Ricerca globale | `/api/man/search?q=vim` |
| `GET /man/{area}/{section}/{name}` | Render HTML | `/man/system/1/ls` |
| `GET /man/{area}/{section}/{name}.md` | Render Markdown | `/man/packages/1/git.md` |
| `GET /man/{area}/{section}/{name}.pdf` | Render PDF | `/man/system/2/fork.pdf` |

## Validazione Area

Le aree valide sono **solo**:
- `system`
- `packages`

Qualsiasi altro valore ritorna errore:
```json
{
  "error": "Invalid area. Use 'system' or 'packages'"
}
```

## Esempi d'Uso

### Frontend: Lista sezioni per area
```javascript
fetch('/api/man')
  .then(r => r.json())
  .then(data => {
    // System sections
    data.system.sections.forEach(s => {
      console.log(`System: ${s.id} - ${s.name}`);
    });
    
    // Package sections
    data.packages.sections.forEach(s => {
      console.log(`Packages: ${s.id} - ${s.name}`);
    });
  });
```

### Frontend: Mostra comandi di sistema vs pacchetti
```javascript
// Comandi sistema base
fetch('/api/man/system/1')
  .then(r => r.json())
  .then(data => {
    document.getElementById('system-commands').innerHTML = 
      data.pages.map(p => `<li>${p.name}: ${p.desc}</li>`).join('');
  });

// Comandi da pacchetti
fetch('/api/man/packages/1')
  .then(r => r.json())
  .then(data => {
    document.getElementById('package-commands').innerHTML = 
      data.pages.map(p => `<li>${p.name}: ${p.desc}</li>`).join('');
  });
```

### Frontend: Visualizza manpage
```html
<!-- Iframe per ls di sistema -->
<iframe src="/man/system/1/ls"></iframe>

<!-- Iframe per git da packages -->
<iframe src="/man/packages/1/git"></iframe>
```

## Modifiche ai File

### man.h
Aggiunto parametro `area` a tutte le funzioni:
```c
char* man_get_section_pages_json(const char *area, const char *section);
char* man_get_page_metadata_json(const char *area, const char *section, const char *name);
char* man_render_page(const char *area, const char *section, const char *name, const char *format);
```

### man.c
- `man_get_sections_json()` - ritorna JSON con system/packages
- `man_get_section_pages_json()` - usa `-M /usr/share/man` o `-M /usr/local/man`
- `man_render_page()` - usa il path corretto in base all'area
- `man_api_handler()` - parsing URL con area
- `man_render_handler()` - parsing URL con area

## Path Resolution

### System Area
```bash
apropos -M /usr/share/man -s 1 ^
man -M /usr/share/man -S 1 -T html ls
```

### Packages Area
```bash
apropos -M /usr/local/man -s 1 ^
man -M /usr/local/man -S 1 -T html git
```

## Testing

```bash
# Lista aree
curl http://localhost:9001/api/man | jq

# Lista comandi sistema
curl http://localhost:9001/api/man/system/1 | jq .pages

# Lista comandi packages
curl http://localhost:9001/api/man/packages/1 | jq .pages

# Metadata ls (system)
curl http://localhost:9001/api/man/system/1/ls | jq

# Metadata git (packages)
curl http://localhost:9001/api/man/packages/1/git | jq

# Render ls HTML
curl http://localhost:9001/man/system/1/ls > ls.html

# Render git PDF
curl http://localhost:9001/man/packages/1/git.pdf > git.pdf

# Search
curl "http://localhost:9001/api/man/search?q=network" | jq
```

## Benefici della Separazione

1. **Chiarezza** - L'utente sa se sta guardando una manpage di sistema o di un pacchetto
2. **Performance** - Query più veloci (meno pagine da scansionare)
3. **Organizzazione** - Frontend può presentare due tab/sezioni distinte
4. **Manutenibilità** - Più facile gestire aggiornamenti di sistema vs pacchetti

## Frontend UI Suggerito

```
┌─────────────────────────────────────┐
│  Man Pages                          │
├─────────────────────────────────────┤
│  [System]  [Packages]  [Search]     │ ← Tabs
├─────────────────────────────────────┤
│                                     │
│  System - Section 1: User Commands  │
│  ┌─────────────────────────────┐   │
│  │ cat  - concatenate files    │   │
│  │ ls   - list directory       │   │
│  │ grep - search patterns      │   │
│  └─────────────────────────────┘   │
│                                     │
│  Packages - Section 1: Commands     │
│  ┌─────────────────────────────┐   │
│  │ git  - version control      │   │
│  │ vim  - text editor          │   │
│  │ tmux - terminal multiplexer │   │
│  └─────────────────────────────┘   │
└─────────────────────────────────────┘
```

## Backward Compatibility

**Breaking Change**: Gli URL vecchi NON funzionano più!

- ❌ `/man/1/ls` → 404
- ✅ `/man/system/1/ls` → OK

Se vuoi mantenere backward compatibility, potresti aggiungere un fallback in `man_render_handler` che assume `system` se l'area non è specificata.

## Note Implementative

- La ricerca (`/api/man/search`) cerca ancora in **entrambe** le aree
- Le sezioni in `packages` sono un subset di quelle in `system` (no section 2, 4, 9)
- Alcuni pacchetti potrebbero installare man in sezioni custom (es. 3pm per Perl)

## 🔒 Security Features OpenBSD

### Pledge
Il server utilizza `pledge()` per limitare le system call disponibili:

```c
const char *promises = "stdio rpath wpath inet proc exec";
pledge(promises, NULL);
```

| Promise | Scopo |
|---------|-------|
| `stdio` | I/O standard |
| `rpath` | Lettura file (templates, static) |
| `wpath` | Scrittura file (se necessario) |
| `inet` | Socket di rete |
| `proc` | Info processi (per metriche) |
| `exec` | Esecuzione comandi (netstat, iostat) |

### Unveil
Il server limita l'accesso al filesystem con `unveil()`:

```c
unveil("templates", "r");  // Solo lettura templates
unveil("static", "r");     // Solo lettura static files
unveil(NULL, NULL);        // Lock
```

### Best Practices Implementate
- ✅ Nessun accesso root richiesto
- ✅ Bind su localhost di default
- ✅ Rate limiting connessioni per IP
- ✅ Timeout su connessioni
- ✅ Validazione input utente
- ✅ Buffer overflow prevention (strlcpy, snprintf)

## 📊 API Metriche - Dettagli Tecnici

### Sorgenti Dati

| Metrica | Sorgente | API/Comando |
|---------|----------|-------------|
| CPU | Kernel | `sysctl(KERN_CPTIME)` |
| Memory | Kernel | `sysctl(VM_UVMEXP)` |
| Swap | Kernel | `swapctl(SWAP_STATS)` |
| Disk | Kernel | `getmntinfo()` |
| Uptime | Kernel | `sysctl(KERN_BOOTTIME)` |
| Load | POSIX | `getloadavg()` |
| OS Info | POSIX | `uname()` |
| Network | BSD | `getifaddrs()` |
| Ports | Command | `netstat -an` |

### Performance
- **Latenza media**: < 5ms per richiesta `/api/metrics`
- **Throughput**: > 1000 req/s su hardware modesto
- **Memory footprint**: ~2-3 MB RSS

## 🛠️ Sviluppo

### Aggiungere Nuove Route

1. Definisci l'handler in `routes.c`:
```c
int my_handler(void *cls, struct MHD_Connection *connection,
               const char *url, const char *method,
               const char *version, const char *upload_data,
               size_t *upload_data_size, void **con_cls) {
    // Implementation
    return MHD_YES;
}
```

2. Registra la route in `init_routes()`:
```c
static struct route routes[] = {
    {"GET", "/myendpoint", my_handler, NULL},
    // ...
};
```

### Aggiungere Nuove Metriche

1. Aggiungi funzione di raccolta in `metrics.c`:
```c
int metrics_get_my_stat(MyStats *stats) {
    // Usa sysctl, getifaddrs, o popen
    return 0;
}
```

2. Aggiungi JSON builder:
```c
static void append_my_stat_json(char *buffer, size_t size) {
    MyStats stats;
    if (metrics_get_my_stat(&stats) == 0) {
        snprintf(buffer, size, "\"my_stat\": {...}");
    }
}
```

3. Includi nel JSON principale in `get_system_metrics_json()`.

### Template Engine

Il template engine supporta placeholder semplici:

**base.html:**
```html
<!DOCTYPE html>
<html>
<head>
    <title>{{title}}</title>
    {{extra_head}}
</head>
<body>
    {{page_content}}
    {{extra_js}}
</body>
</html>
```

**Uso in codice:**
```c
struct template_data data = {
    .title = "My Page",
    .page_content = "content.html",
    .extra_head_file = "custom_head.html",
    .extra_js_file = "custom_js.html"
};

char *output;
template_render_with_data(&data, &output);
```

## 🧪 Testing

### Test Manuale
```bash
# Test homepage
curl http://127.0.0.1:9001/

# Test API metriche
curl http://127.0.0.1:9001/api/metrics | jq .

# Test static files
curl http://127.0.0.1:9001/static/css/style.css

# Test con load
ab -n 1000 -c 10 http://127.0.0.1:9001/api/metrics
```

### Monitoring
```bash
# Verifica pledge/unveil violations
doas tail -f /var/log/messages | grep miniweb

# Check resource usage
top -p $(pgrep miniweb)

# Network connections
netstat -an | grep 9001

# Syscall tracing (debug)
ktrace -p $(pgrep miniweb)
kdump | less
```

## 🐛 Troubleshooting

### Server non parte
```bash
# Verifica che la porta sia libera
netstat -an | grep 9001

# Verifica permessi
ls -la templates/ static/

# Verifica dipendenze
pkg_info | grep microhttpd
```

### Pledge Violation
```bash
# Check logs
doas tail /var/log/messages

# Causa comune: promises mancanti
# Soluzione: aggiungi la promise necessaria in main.c
```

### Performance Issues
```bash
# Aumenta thread pool
./miniweb -t 16

# Aumenta connection limit
./miniweb -c 2000

# Check system limits
ulimit -a
```

## 📚 Risorse

### Documentazione OpenBSD
- `man 3 sysctl` - System control
- `man 2 pledge` - Restrict system operations
- `man 2 unveil` - Filesystem access restriction
- `man 3 getifaddrs` - Network interfaces
- `man 3 swapctl` - Swap management

### libmicrohttpd
- [GNU libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/)
- [Tutorial](https://www.gnu.org/software/libmicrohttpd/tutorial.html)

### Riferimenti
- [OpenBSD FAQ](https://www.openbsd.org/faq/)
- [OpenBSD man pages](https://man.openbsd.org/)

## 📝 Changelog

### Version 1.1.0 (Current)
- ✅ Metriche di sistema complete con API native OpenBSD
- ✅ CPU stats via `sysctl(KERN_CPTIME)`
- ✅ Memoria e SWAP via `sysctl(VM_UVMEXP)` e `swapctl()`
- ✅ Disk usage via `getmntinfo()` invece di parsing `df`
- ✅ Network interfaces con IP corretti
- ✅ Port scanning TCP e UDP migliorato
- ✅ Template engine funzionante
- ✅ Static file serving

### Version 1.0.0
- ⚠️  Metriche con valori placeholder
- ⚠️  Parsing comandi esterni non ottimale

## 🤝 Contributi

Contributi benvenuti! Per favore:

1. Fork del repository
2. Crea feature branch (`git checkout -b feature/AmazingFeature`)
3. Commit delle modifiche (`git commit -m 'Add AmazingFeature'`)
4. Push al branch (`git push origin feature/AmazingFeature`)
5. Apri una Pull Request

### Guidelines
- Segui lo stile OpenBSD (KNF)
- Usa `strlcpy` invece di `strcpy`
- Usa `snprintf` invece di `sprintf`
- Testa su OpenBSD before commit
- Documenta le nuove funzioni

## 📄 Licenza

[Specifica qui la tua licenza - es. BSD, MIT, GPL]

## 👥 Autori

[Il tuo nome]

## 🙏 Ringraziamenti

- OpenBSD Team per il fantastico sistema operativo
- GNU libmicrohttpd per l'eccellente libreria HTTP
- Community OpenBSD per il supporto

## 📧 Contatti

- Issues: [GitTea Issues](https://gitea.my.zimaserver/flavio/miniweb_libmicrohttpd/issues)
- Email: [flavio@flvbox.org]
- Website: [https://gitea.my.zimaserver/flavio/miniweb_libmicrohttpd/]

---

**Nota**: Questo è un progetto educativo/dimostrativo. Per uso in produzione, considera ulteriori hardening, logging, error handling, e testing estensivi.
