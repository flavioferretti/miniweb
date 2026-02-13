# MiniWeb - OpenBSD Secure Web Server

Web server ultra-leggero e blindato per OpenBSD, costruito con *libmicrohttpd*. 
Progettato per essere aderente alla filosofia KNF (Kernel Normal Form) e protetto dalle feature native del sistema operativo.

## 🚀 Opzioni di Avvio (CLI)

Il binario build/miniweb supporta le seguenti configurazioni:

$ build/miniweb -h
Usage: build/miniweb [options]
Options:
  -p PORT      Port to listen on (default: 9001)
  -b ADDR      Address to bind to (default: 127.0.0.1)
  -t NUM       Thread pool size (default: 4)
  -c NUM       Max connections (default: 1000)
  -v           Enable verbose output
  -h           Show this help

## 📋 Documentazione API (REST Reference)

Il server espone i seguenti endpoint per l'integrazione con dashboard esterne o monitoraggio.

### 1. System Metrics
Endpoint: GET /api/metrics
Restituisce i parametri vitali del sistema in tempo reale.
- Caching: Dati protetti da Mutex con TTL di 2 secondi (previene il sovraccarico di sysctl).
- Dati inclusi: CPU (user/sys/idle), UVM Memory (active/wired/free), Swap, Load Average, Disk Usage, Network Interfaces e Stato delle Porte.

### 2. Manual Pages (Man API)
Endpoint: /api/man/
Interfaccia programmatica per la documentazione di sistema.
- GET /api/man/sections: Elenco sezioni (1-9).
- GET /api/man/pages?section=X: Pagine disponibili nella sezione X.
- GET /api/man/search?q=query: Ricerca nel database man.
- Rendering: Visualizzazione pagine via /man/{area}/{section}/{page} in formato HTML.

### 3. Static Assets
- GET /static/*: Serve CSS, JS, e immagini con MIME types corretti.
- GET /: Dashboard principale renderizzata dal template engine interno.

## 🛠️ Ultime Migliorie Tecniche

- Thread Safety: Accesso alle metriche sincronizzato via pthread_mutex_t.
- Global Logging: Macro LOG integrata con flag globale extern config_verbose per eliminare i warning.
- Hardening: Utilizzo di pledge() per limitare le syscall e unveil() per isolare il filesystem.

## 🔧 Compilazione (OpenBSD Make)

$ make clean && make

# Per il debug (senza ottimizzazioni)
$ make debug

---
Autore: Flavio
Piattaforma: OpenBSD 7.x
Licenza: BSD 3-Clause
