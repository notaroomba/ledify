/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{js,jsx}'],
  theme: {
    extend: {
      colors: {
        ink: '#07080b', surface: '#0f1116', raised: '#171a21', line: '#252a35',
        fg: '#eef0f4', muted: '#8b93a5', accent: 'rgb(var(--accent) / <alpha-value>)', ok: '#3ddc84', warn: '#f5b73a', bad: '#ff6b6b',
      },
      fontFamily: {
        sans: ['Inter', 'ui-sans-serif', 'system-ui', '-apple-system', 'Segoe UI', 'Roboto', 'sans-serif'],
        display: ['"Space Grotesk"', 'Inter', 'ui-sans-serif', 'system-ui', 'sans-serif'],
      },
    },
  },
  plugins: [],
};
