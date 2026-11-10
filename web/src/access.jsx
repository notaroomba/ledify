// Who may use which board (the hosted panel with accounts on), and the board's own page offering to link this
// browser while its setup window is open. The server keeps the lists; see web/server/index.mjs.
import { For, Show, createSignal, createEffect } from 'solid-js';
import { ui, api, authConnect, toast, boardLabel, AUTH, LOCAL } from './store';
import { setSetupOpen } from './dialogs';
import { Group, confirmDialog } from './ui';

export const HOSTED = import.meta.env.VITE_HOSTED_URL || 'https://ledify.notaroomba.dev';
const ago = t => { if (!t) return 'never'; const m = Math.round((Date.now() - t) / 60000); return m < 2 ? 'just now' : m < 120 ? m + ' min ago' : m < 2880 ? Math.round(m / 60) + ' h ago' : Math.round(m / 1440) + ' days ago'; };

export function OwnerLogin() {
  const [pass, setPass] = createSignal(''), [remember, setRemember] = createSignal(true), [msg, setMsg] = createSignal('');
  const go = async e => {
    e.preventDefault();
    try { await api('/api/login', { password: pass(), remember: remember() }); setPass(''); setMsg(''); await authConnect(true); toast('Logged in', 'info'); }
    catch (err) { setMsg(err.message); }
  };
  return <form class="grid gap-2" onSubmit={go}>
    <input type="password" class="field" autocomplete="current-password" placeholder="owner password" aria-label="Owner password" value={pass()} onInput={e => setPass(e.currentTarget.value)} />
    <label class="hint flex items-center gap-2"><input type="checkbox" checked={remember()} onChange={e => setRemember(e.currentTarget.checked)} />Remember me on this device for 90 days</label>
    <div class="row"><button type="submit" class="btn btn-primary btn-sm">Log in</button><span class="hint">{msg()}</span></div>
  </form>;
}

// First visit (or every board removed): how to link one. Nothing else on the page works until then.
export function LinkGate() {
  const show = () => AUTH && ui.me && !ui.me.owner && !ui.me.boards.length;
  return <Show when={show()}>
    <div class="gate">
      <div class="gate-card">
        <h2>Link a board to this browser</h2>
        <ol class="gate-steps">
          <li>Hold the board's button for 3 seconds, until the strip flashes blue three times. The board then accepts a link for 2 minutes.</li>
          <li>Then either:
            <ul class="mt-1 list-disc space-y-1 pl-5">
              <li><b>Link over Bluetooth</b> below and pick the board (Chrome or Edge on a computer or Android phone), or</li>
              <li>on the same Wi-Fi as the board, open <b>http://ledify.local</b> (or the board's name followed by .local) and tap <b>Link this browser</b>. It brings you back here, linked.</li>
            </ul>
          </li>
          <li>Or open a share link the board's owner sent you.</li>
        </ol>
        <button type="button" class="btn btn-primary" onClick={() => setSetupOpen(true)}>Link over Bluetooth</button>
        <p class="hint">This browser remembers its boards with a cookie, so you only do this once.</p>
        <details class="mt-4"><summary>Owner login</summary><div class="mt-2"><OwnerLogin /></div></details>
      </div>
    </div>
  </Show>;
}

// The board's own page while its setup window is open: hand the code to the hosted panel.
export function PairBanner() {
  const url = () => ui.pair && HOSTED + '/#link=' + ui.pair.id + '.' + ui.pair.code;
  const copy = () => navigator.clipboard?.writeText(url()).then(() => toast('Link copied: open it on the phone or computer to link', 'info', 5000));
  return <Show when={LOCAL && ui.pair}>
    <div class="pair-banner">
      <span class="grow">Setup is open for 2 minutes. Link a browser to this board on the hosted panel:</span>
      <a class="btn btn-primary btn-sm" href={url()} target="_blank" rel="noopener">Link this browser</a>
      <button type="button" class="btn btn-sm" onClick={copy}>Copy link</button>
    </div>
  </Show>;
}

export const [accessOpen, setAccessOpen] = createSignal(false);
export function Access() {
  let d;
  const [list, setList] = createSignal(null), [label, setLabel] = createSignal(''), [share, setShare] = createSignal(null);
  const load = async () => {
    setLabel(ui.me?.label || '');
    if (ui.me?.owner) { try { setList((await api('/api/access')).boards); } catch (e) { toast(e.message, 'warn'); } }
  };
  createEffect(() => { if (accessOpen()) { if (!d.open) d.showModal(); setShare(null); load(); } else if (d.open) d.close(); });
  const saveLabel = async () => { try { await api('/api/label', { label: label() }); toast('Saved', 'info'); } catch (e) { toast(e.message, 'warn'); } };
  const makeShare = async id => {
    try {
      const r = await api('/api/share', { board: id });
      const url = location.origin + '/#join=' + r.token;
      setShare({ id, url });
      navigator.clipboard?.writeText(url).then(() => toast('Share link copied (single use, ' + r.hours + ' h)', 'info', 5000));
    } catch (e) { toast(e.message, 'warn'); }
  };
  const revoke = async (id, dv, who) => {
    if (!(await confirmDialog('Remove access?', (who || 'This browser') + ' will no longer see or control ' + boardLabel(id) + '. They can be linked again in person or with a new share link.', 'Remove'))) return;
    try { await api('/api/revoke', { board: id, device: dv }); await load(); await authConnect(true); } catch (e) { toast(e.message, 'warn'); }
  };
  const forget = async id => {
    if (!(await confirmDialog('Forget this board?', 'Its broker login is deleted and everyone loses access. It stays offline until someone links it again by holding its button.', 'Forget'))) return;
    try { await api('/api/forget', { board: id }); await load(); await authConnect(true); } catch (e) { toast(e.message, 'warn'); }
  };
  const logout = async () => { try { await api('/api/logout', {}); setAccessOpen(false); await authConnect(true); } catch (e) { toast(e.message, 'warn'); } };
  return <dialog ref={d} class="dialog" onClose={() => setAccessOpen(false)}>
    <h3>Access</h3>
    <Group title="This browser">
      <div class="row"><input type="text" class="field grow" maxLength={40} placeholder="name it, e.g. Sam's phone" aria-label="This browser's name" value={label()} onInput={e => setLabel(e.currentTarget.value)} /><button type="button" class="btn btn-sm" onClick={saveLabel}>Save</button></div>
      <Show when={!ui.me?.owner}>
        <For each={ui.me?.boards || []}>{id => <div class="row mt-2"><span class="grow">{boardLabel(id)}</span><button type="button" class="btn btn-sm" onClick={() => revoke(id)}>Leave</button></div>}</For>
        <details class="mt-3"><summary>Owner login</summary><div class="mt-2"><OwnerLogin /></div></details>
      </Show>
    </Group>
    <Show when={ui.me?.owner}>
      <For each={list() || []} fallback={<p class="hint">No boards linked yet. Hold a board's button for 3 seconds and link it from its own page.</p>}>{b => <Group title={b.name && b.name !== 'ledify' ? b.name : b.id} aside={b.id}>
        <For each={b.devices} fallback={<p class="hint">Only you.</p>}>{v => <div class="row">
          <span class="grow truncate">{v.label || 'unnamed browser'}{v.you ? ' (this one)' : ''}</span><span class="hint">{ago(v.seen)}</span>
          <button type="button" class="btn btn-sm" onClick={() => revoke(b.id, v.device, v.label)}>Remove</button>
        </div>}</For>
        <div class="row mt-2">
          <button type="button" class="btn btn-sm btn-primary" onClick={() => makeShare(b.id)}>Make a share link</button>
          <button type="button" class="btn btn-sm" onClick={() => forget(b.id)}>Forget board</button>
        </div>
        <Show when={share()?.id === b.id}><input class="field mt-2 w-full" readOnly value={share().url} aria-label="Share link" onFocus={e => e.currentTarget.select()} /></Show>
      </Group>}</For>
      <div class="mt-3 flex justify-end"><button type="button" class="btn btn-sm" onClick={logout}>Log out</button></div>
    </Show>
    <div class="mt-3 flex justify-end"><button type="button" class="btn" onClick={() => setAccessOpen(false)}>Close</button></div>
  </dialog>;
}
