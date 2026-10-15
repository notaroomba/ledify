/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{js,jsx}'],
  theme: {
    extend: {
      colors: {
        ink: '#0a0c10', surface: '#12151c', raised: '#1a1e27', line: '#242a36',
        fg: '#e9ecf1', muted: '#8a93a6', accent: '#58c6ff', ok: '#3ddc84', warn: '#f5b73a', bad: '#ff6b6b',
      },
      fontFamily: { sans: ['Inter', 'ui-sans-serif', 'system-ui', '-apple-system', 'Segoe UI', 'Roboto', 'sans-serif'] },
      keyframes: { pop: { '0%': { opacity: 0, transform: 'translateY(8px) scale(.98)' }, '100%': { opacity: 1, transform: 'none' } } },
      animation: { pop: 'pop .18s ease-out' },
    },
  },
  plugins: [],
};
