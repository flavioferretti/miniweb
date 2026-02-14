  (() => {
      const root = document.documentElement;
      const toggle = document.getElementById('themeToggle');
      const menuToggle = document.getElementById('navbarToggle');
      const menu = document.getElementById('navbarMenu');

      const applyTheme = (theme) => {
          if (theme === 'auto') {
              const prefersDark = window.matchMedia('(prefers-color-scheme: dark)').matches;
              root.setAttribute('data-theme', prefersDark ? 'dark' : 'light');
          } else {
              root.setAttribute('data-theme', theme);
          }
      };

      const savedTheme = localStorage.getItem('miniweb-theme') || 'auto';
      applyTheme(savedTheme);

      toggle?.addEventListener('click', () => {
          const current = root.getAttribute('data-theme') === 'dark' ? 'dark' : 'light';
          const next = current === 'dark' ? 'light' : 'dark';
          localStorage.setItem('miniweb-theme', next);
          applyTheme(next);
      });

      menuToggle?.addEventListener('click', () => {
          const expanded = menuToggle.getAttribute('aria-expanded') === 'true';
          menuToggle.setAttribute('aria-expanded', String(!expanded));
          menu.classList.toggle('show', !expanded);
      });

      const currentPath = window.location.pathname;
      document.querySelectorAll('.nav-link').forEach((link) => {
          if (link.dataset.page === currentPath) {
              link.classList.add('active');
          }
          link.addEventListener('click', () => {
              menu.classList.remove('show');
              menuToggle?.setAttribute('aria-expanded', 'false');
          });
      });
  })();
