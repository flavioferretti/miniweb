(() => {
    const root = document.documentElement;
    const toggle = document.getElementById('themeToggle');

    // 1. Funzione per applicare il tema e aggiornare l'attributo HTML
    const applyTheme = (theme) => {
        let themeToApply = theme;

        if (theme === 'auto') {
            const prefersDark = window.matchMedia('(prefers-color-scheme: dark)').matches;
            themeToApply = prefersDark ? 'dark' : 'light';
        }

        root.setAttribute('data-theme', themeToApply);
        // Salviamo la scelta specifica (dark o light) nel localStorage
        localStorage.setItem(themeKey, themeToApply);
    };

    // 2. Al caricamento: Recupera dal localStorage o usa il default
    // Nota: se il localStorage è vuoto, usiamo 'auto' o 'light'
    /* Key is per-host so each MiniWeb instance remembers its own theme */
    const themeKey = 'miniweb-theme:' + location.hostname + (location.port ? ':' + location.port : '');
    const savedTheme = localStorage.getItem(themeKey) || 'auto';
    applyTheme(savedTheme);

    // 3. Gestione del Click sul pulsante
    toggle?.addEventListener('click', () => {
        // Leggiamo cosa c'è scritto attualmente sull'HTML
        const currentTheme = root.getAttribute('data-theme');
        const nextTheme = currentTheme === 'dark' ? 'light' : 'dark';

        applyTheme(nextTheme);
    });

})();
