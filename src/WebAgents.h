#pragma once

// Scripts run inside the signed-in sync browser, one per service. Each reads
// the conversation list from the site's own backend (the same requests the
// website makes) and talks to Sessions through window.chrome.webview:
//
//   → {type:"status", text}                 progress for the status line
//   → {type:"signedOut"}                     the browser is not signed in
//   → {type:"list", account, accountName, complete, items:[{id, updated}]}
//   ← {type:"fetch", ids:[...]}              which conversations to read
//   → {type:"conversation", id, account, title, project, created, updated, messages:[{role, text, ts}]}
//   → {type:"error", text}
//   → {type:"done"}
//
// These backends are not public interfaces and change from time to time; when
// a sync reports an error, this is the file to fix.
namespace sessions::agents {

// Shared helpers, prepended to every agent.
constexpr wchar_t Common[] = LR"JS(
const post = (m) => window.chrome.webview.postMessage(m);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fetchWaiter = null;
window.chrome.webview.addEventListener('message', (e) => {
  const m = e.data;
  if (m && m.type === 'fetch' && fetchWaiter) { const w = fetchWaiter; fetchWaiter = null; w(m.ids || []); }
});
const askWhichToFetch = (list) => new Promise((resolve) => { fetchWaiter = resolve; post(list); });
const toIso = (v) => {
  if (v === undefined || v === null || v === '') return '';
  if (typeof v === 'number') return new Date(v < 1e12 ? v * 1000 : v).toISOString();
  return String(v);
};
async function getJson(url, init) {
  for (let attempt = 0; attempt < 4; attempt++) {
    const r = await fetch(url, Object.assign({ credentials: 'include' }, init || {}));
    if (r.status === 429) { await sleep(5000 * (attempt + 1)); continue; }
    if (r.status === 401 || r.status === 403) { const e = new Error('signed out'); e.signedOut = true; throw e; }
    if (!r.ok) throw new Error(url.split('?')[0] + ' answered ' + r.status);
    return r.json();
  }
  throw new Error(url.split('?')[0] + ' kept answering 429 (too many requests)');
}
)JS";

constexpr wchar_t Claude[] = LR"JS(
(async () => {
  try {
    const textOf = (m) => {
      if (Array.isArray(m.content) && m.content.length) {
        return m.content.filter((c) => c && c.type === 'text' && c.text).map((c) => c.text).join('\n\n');
      }
      return m.text || '';
    };
    const orgs = await getJson('/api/organizations');
    for (const org of orgs) {
      const caps = org.capabilities || [];
      if (caps.length && !caps.includes('chat')) continue;
      const account = 'claude:' + org.uuid;
      post({ type: 'status', text: 'Claude · ' + (org.name || 'organization') + ' · listing' });
      let items = [];
      try {
        items = await getJson('/api/organizations/' + org.uuid + '/chat_conversations');
      } catch (e) {
        if (e.signedOut) throw e;
        post({ type: 'error', text: 'Claude ' + (org.name || org.uuid) + ': ' + e.message });
        continue;
      }
      if (!Array.isArray(items)) items = items.data || [];
      const ids = await askWhichToFetch({
        type: 'list', account, accountName: org.name || '', complete: true,
        items: items.map((c) => ({ id: 'claude:' + c.uuid, updated: toIso(c.updated_at) })),
      });
      let n = 0;
      for (const full of ids) {
        const id = full.replace(/^claude:/, '');
        try {
          const c = await getJson('/api/organizations/' + org.uuid + '/chat_conversations/' + id +
                                  '?tree=True&rendering_mode=messages&render_all_tools=true');
          const messages = (c.chat_messages || [])
            .map((m) => ({ role: m.sender === 'human' ? 'user' : 'assistant', ts: toIso(m.created_at), text: textOf(m) }))
            .filter((m) => m.text);
          post({ type: 'conversation', id: full, account, title: c.name || '', project: (c.project && c.project.name) || '',
                 created: toIso(c.created_at), updated: toIso(c.updated_at), messages });
        } catch (e) {
          if (e.signedOut) throw e;
          post({ type: 'error', text: 'Claude conversation ' + id + ': ' + e.message });
        }
        if (++n % 10 === 0) post({ type: 'status', text: 'Claude · ' + (org.name || '') + ' · ' + n + ' of ' + ids.length });
        await sleep(200);
      }
    }
    post({ type: 'done' });
  } catch (e) {
    post(e.signedOut ? { type: 'signedOut' } : { type: 'error', text: 'Claude: ' + e.message, fatal: true });
  }
})();
)JS";

constexpr wchar_t ChatGpt[] = LR"JS(
(async () => {
  try {
    const session = await getJson('/api/auth/session');
    if (!session || !session.accessToken) { post({ type: 'signedOut' }); return; }
    const auth = { Authorization: 'Bearer ' + session.accessToken };
    // Personal account and any workspaces.
    let accounts = [];
    try {
      const check = await getJson('/backend-api/accounts/check/v4-2023-04-27', { headers: auth });
      for (const [key, value] of Object.entries(check.accounts || {})) {
        const a = (value && value.account) || {};
        const id = a.account_id || key;
        if (id && id !== 'default') accounts.push({ id, name: a.name || (a.structure === 'personal' ? 'Personal' : '') });
      }
    } catch (e) { if (e.signedOut) throw e; }
    if (!accounts.length) accounts.push({ id: '', name: '' });

    const textOf = (message) => {
      const c = message.content || {};
      if (Array.isArray(c.parts)) return c.parts.filter((p) => typeof p === 'string').join('\n\n');
      if (typeof c.text === 'string') return c.text;
      return '';
    };
    for (const acct of accounts) {
      const headers = Object.assign({}, auth, acct.id ? { 'ChatGPT-Account-Id': acct.id } : {});
      const account = 'codex:' + (acct.id || (session.user && session.user.id) || '');
      const items = [];
      const projectOf = {};
      post({ type: 'status', text: 'ChatGPT · listing' });
      for (const archived of [false, true]) {
        for (let offset = 0; ; offset += 100) {
          const page = await getJson('/backend-api/conversations?offset=' + offset + '&limit=100&order=updated&is_archived=' + archived, { headers });
          const list = page.items || [];
          for (const c of list) items.push({ id: 'chatgpt:' + c.id, updated: toIso(c.update_time) });
          if (list.length < 100) break;
          await sleep(300);
        }
      }
      // Conversations inside projects are listed per project.
      try {
        const sidebar = await getJson('/backend-api/gizmos/snorlax/sidebar?conversations_per_gizmo=0', { headers });
        for (const entry of sidebar.items || []) {
          const g = (entry.gizmo && entry.gizmo.gizmo) || entry.gizmo || {};
          if (!g.id) continue;
          const name = (g.display && g.display.name) || '';
          let cursor = '0';
          while (cursor !== null && cursor !== undefined) {
            const page = await getJson('/backend-api/gizmos/' + g.id + '/conversations?cursor=' + encodeURIComponent(cursor), { headers });
            for (const c of page.items || []) {
              items.push({ id: 'chatgpt:' + c.id, updated: toIso(c.update_time) });
              projectOf[c.id] = name;
            }
            cursor = page.cursor;
            await sleep(300);
          }
        }
      } catch (e) { if (e.signedOut) throw e; post({ type: 'error', text: 'ChatGPT projects: ' + e.message }); }

      const ids = await askWhichToFetch({ type: 'list', account, accountName: acct.name, complete: true, items });
      let n = 0;
      for (const full of ids) {
        const id = full.replace(/^chatgpt:/, '');
        try {
          const c = await getJson('/backend-api/conversation/' + id, { headers });
          // Follow the branch that is shown, from the current message back to the start.
          const chain = [];
          let node = c.current_node;
          while (node && c.mapping && c.mapping[node]) {
            const m = c.mapping[node].message;
            if (m) chain.push(m);
            node = c.mapping[node].parent;
          }
          chain.reverse();
          const messages = chain
            .filter((m) => m.author && (m.author.role === 'user' || m.author.role === 'assistant'))
            .filter((m) => !(m.metadata && m.metadata.is_visually_hidden_from_conversation))
            .map((m) => ({ role: m.author.role, ts: toIso(m.create_time), text: textOf(m) }))
            .filter((m) => m.text && m.text.trim());
          post({ type: 'conversation', id: full, account, title: c.title || '', project: projectOf[id] || '',
                 created: toIso(c.create_time), updated: toIso(c.update_time), messages });
        } catch (e) {
          if (e.signedOut) throw e;
          post({ type: 'error', text: 'ChatGPT conversation ' + id + ': ' + e.message });
        }
        if (++n % 10 === 0) post({ type: 'status', text: 'ChatGPT · ' + n + ' of ' + ids.length });
        await sleep(400);
      }
    }
    post({ type: 'done' });
  } catch (e) {
    post(e.signedOut ? { type: 'signedOut' } : { type: 'error', text: 'ChatGPT: ' + e.message, fatal: true });
  }
})();
)JS";

// Gemini has no JSON API; the web app talks to "batchexecute" with
// positional arrays. MaZiqc lists chats, hNvQHb reads one.
constexpr wchar_t Gemini[] = LR"JS(
(async () => {
  try {
    const W = window.WIZ_global_data || {};
    if (!W.SNlM0e) { post({ type: 'signedOut' }); return; }
    let reqid = Math.floor(Math.random() * 90000) + 10000;
    async function rpc(id, payload) {
      reqid += 100000;
      const body = new URLSearchParams({ 'f.req': JSON.stringify([[[id, JSON.stringify(payload), null, 'generic']]]), at: W.SNlM0e });
      const url = '/_/BardChatUi/data/batchexecute?rpcids=' + id + '&source-path=%2Fapp&bl=' + encodeURIComponent(W.cfb2h || '') +
                  '&f.sid=' + encodeURIComponent(W.FdrFJe || '') + '&hl=en&_reqid=' + reqid + '&rt=c';
      const r = await fetch(url, { method: 'POST', body, credentials: 'include',
                                   headers: { 'content-type': 'application/x-www-form-urlencoded;charset=UTF-8' } });
      if (r.status === 401 || r.status === 403) { const e = new Error('signed out'); e.signedOut = true; throw e; }
      if (!r.ok) throw new Error(id + ' answered ' + r.status);
      const text = await r.text();
      for (const line of text.split('\n')) {
        if (!line.includes('"wrb.fr"')) continue;
        let rows;
        try { rows = JSON.parse(line); } catch (e) { continue; }
        for (const row of rows) if (row[0] === 'wrb.fr' && row[1] === id) return row[2] ? JSON.parse(row[2]) : null;
      }
      return null;
    }
    const seconds = (t) => (Array.isArray(t) && typeof t[0] === 'number' ? new Date(t[0] * 1000).toISOString() : '');
    const account = 'gemini:' + (W.oPEP7c || 'account');
    post({ type: 'status', text: 'Gemini · listing' });
    const items = [];
    const titles = {};
    let cursor = null;
    for (let page = 0; page < 200; page++) {
      const res = await rpc('MaZiqc', [13, cursor, [0, null, 1]]);
      const chats = (res && Array.isArray(res[2]) ? res[2] : []).filter((c) => Array.isArray(c) && typeof c[0] === 'string');
      for (const c of chats) {
        const updated = seconds(c[5]) || seconds(c[4]) || '';
        items.push({ id: 'gemini:' + c[0], updated });
        titles[c[0]] = c[1] || '';
      }
      cursor = res && typeof res[1] === 'string' && res[1] ? res[1] : null;
      if (!cursor || !chats.length) break;
      await sleep(300);
    }
    const ids = await askWhichToFetch({ type: 'list', account, accountName: W.oPEP7c || '', complete: items.length > 0, items });
    let n = 0;
    for (const full of ids) {
      const id = full.replace(/^gemini:/, '');
      try {
        const res = await rpc('hNvQHb', [id, 10, null, 1, [0], [4], null, 1]);
        const turns = (res && Array.isArray(res[0]) ? res[0] : []).slice().reverse();
        const messages = [];
        let created = '', updated = '';
        for (const turn of turns) {
          const ts = seconds(turn[4]);
          if (ts && !created) created = ts;
          if (ts) updated = ts;
          const user = turn && turn[2] && turn[2][0] && turn[2][0][0];
          const reply = turn && turn[3] && turn[3][0] && turn[3][0][0] && turn[3][0][0][1] && turn[3][0][0][1][0];
          if (typeof user === 'string' && user.trim()) messages.push({ role: 'user', ts, text: user });
          if (typeof reply === 'string' && reply.trim()) messages.push({ role: 'assistant', ts, text: reply });
        }
        post({ type: 'conversation', id: full, account, title: titles[id] || '', project: '', created, updated, messages });
      } catch (e) {
        if (e.signedOut) throw e;
        post({ type: 'error', text: 'Gemini conversation ' + id + ': ' + e.message });
      }
      if (++n % 10 === 0) post({ type: 'status', text: 'Gemini · ' + n + ' of ' + ids.length });
      await sleep(400);
    }
    post({ type: 'done' });
  } catch (e) {
    post(e.signedOut ? { type: 'signedOut' } : { type: 'error', text: 'Gemini: ' + e.message, fatal: true });
  }
})();
)JS";

}  // namespace sessions::agents
