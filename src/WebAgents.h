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
// Set when a site rate-limits; the reading loops wait longer between requests while it is high.
let throttle = 0;
// `patient`: keep waiting out "too many requests" (up to about 12 hours) instead of giving up.
async function getJson(url, init, patient) {
  for (let attempt = 0; attempt < (patient ? 60 : 4); attempt++) {
    const abort = new AbortController();
    const timer = setTimeout(() => abort.abort(), 60000);
    let r;
    try {
      r = await fetch(url, Object.assign({ credentials: 'include', signal: abort.signal }, init || {}));
    } catch (e) {
      clearTimeout(timer);
      if (attempt < 3) { await sleep(5000); continue; }
      throw new Error(url.split('?')[0] + ' did not answer');
    }
    clearTimeout(timer);
    // Too many requests: back off well before trying again. The site's limit is shared
    // with your own browser, so pressing on would also stop chats loading for you.
    // Too many requests: stop and leave the site alone for a while. Its limit is shared
    // with your own browser, so pressing on would also stop chats loading for you.
    if (r.status === 429) {
      const after = parseInt(r.headers.get('retry-after') || '', 10);
      const e = new Error(location.hostname.replace(/^www\./, '') + ' asked Sessions to slow down');
      e.rateLimited = true;
      e.seconds = after > 0 ? after : 0;
      throw e;
    }
    if (r.status === 401 || r.status === 403) {
      let detail = '';
      try { detail = (await r.text()).replace(/\s+/g, ' ').slice(0, 300); } catch (e) {}
      const e = new Error(url.split('?')[0] + ' answered ' + r.status + (detail ? ': ' + detail : ''));
      e.signedOut = true;
      throw e;
    }
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
    post({ type: 'identity', who: orgs.map((o) => o.name).filter(Boolean).join(', ') });
    for (const org of orgs) {
      const caps = org.capabilities || [];
      if (caps.length && !caps.includes('chat')) continue;
      const account = 'claude:' + org.uuid;
      post({ type: 'status', text: 'Claude · ' + (org.name || 'organization') + ' · listing' });
      let items = [];
      try {
        const seen = new Set();
        for (let offset = 0; offset < 100000; offset += 100) {
          let page = await getJson('/api/organizations/' + org.uuid + '/chat_conversations?limit=100&offset=' + offset);
          if (!Array.isArray(page)) page = page.data || [];
          let added = 0;
          for (const c of page) if (c && c.uuid && !seen.has(c.uuid)) { seen.add(c.uuid); items.push(c); added++; }
          // Fewer than a page, or the site ignored paging and sent everything again.
          if (page.length < 100 || !added) break;
          await sleep(200);
        }
      } catch (e) {
        if (e.signedOut || e.rateLimited) throw e;
        post({ type: 'error', text: 'Claude ' + (org.name || org.uuid) + ': ' + e.message });
        continue;
      }
      const ids = await askWhichToFetch({
        type: 'list', account, accountName: org.name || '', complete: true,
        items: items.map((c) => ({ id: 'claude:' + c.uuid, updated: toIso(c.updated_at), created: toIso(c.created_at),
                                   title: c.name || '', project: (c.project && c.project.name) || '' })),
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
          if (e.signedOut || e.rateLimited) throw e;
          post({ type: 'error', text: 'Claude conversation ' + id + ': ' + e.message });
        }
        if (++n % 10 === 0) post({ type: 'status', text: 'Claude · ' + (org.name || '') + ' · ' + n + ' of ' + ids.length });
        await sleep(200);
      }
    }
    post({ type: 'done' });
  } catch (e) {
    post(e.rateLimited ? { type: 'rateLimited', seconds: e.seconds || 0 } : e.signedOut ? { type: 'signedOut', detail: e.message } : { type: 'error', text: 'Claude: ' + e.message, fatal: true });
  }
})();
)JS";

constexpr wchar_t ChatGpt[] = LR"JS(
(async () => {
  try {
    const session = await getJson('/api/auth/session');
    if (!session || !session.accessToken) { post({ type: 'signedOut', detail: 'no access token in /api/auth/session' }); return; }
    const auth = { Authorization: 'Bearer ' + session.accessToken };
    post({ type: 'identity', who: (session.user && (session.user.email || session.user.name)) || '' });
    // Personal account and any workspaces.
    let accounts = [];
    try {
      const check = await getJson('/backend-api/accounts/check/v4-2023-04-27', { headers: auth });
      for (const [key, value] of Object.entries(check.accounts || {})) {
        const a = (value && value.account) || {};
        const id = a.account_id || (key === 'default' ? '' : key);
        // The personal account may have no id of its own; it is then read without the account header.
        if (!accounts.some((x) => x.id === id)) accounts.push({ id, name: a.name || (a.structure === 'personal' ? 'Personal' : '') });
      }
    } catch (e) { if (e.signedOut || e.rateLimited) throw e; }
    if (!accounts.length) accounts.push({ id: '', name: '' });

    const textOf = (message) => {
      const c = message.content || {};
      // Voice chats keep what was said as {content_type: 'audio_transcription', text}.
      if (Array.isArray(c.parts)) return c.parts.map((p) => typeof p === 'string' ? p : (p && typeof p.text === 'string' ? p.text : ''))
                                               .filter(Boolean).join('\n\n');
      if (typeof c.text === 'string') return c.text;
      return '';
    };
    for (const acct of accounts) {
      const headers = Object.assign({}, auth, acct.id ? { 'ChatGPT-Account-Id': acct.id } : {});
      const account = 'codex:' + (acct.id || (session.user && session.user.id) || '');
      let items = [];
      let projectOf = {};
      // Sessions reads a long history in batches and comes back for more; the list
      // is kept for half an hour in this tab so each return doesn't list it again.
      const cacheKey = 'sessions-list-' + account;
      let cached = null;
      try { cached = JSON.parse(sessionStorage.getItem(cacheKey) || 'null'); } catch (e) {}
      // Sessions has the rest: list only what changed since its newest chat (a day's margin).
      const partial = !sessionsHint.full && sessionsHint.since > 0;
      const since = (sessionsHint.since || 0) - 24 * 3600 * 1000;
      // The list is newest first: once one chat on a page is older than that, the rest are too.
      const older = (list) => list.some((c) => new Date(toIso(c.update_time)).getTime() < since);
      if (cached && Date.now() - cached.at < 30 * 60 * 1000 && (partial || cached.full)) {
        items = cached.items; projectOf = cached.projectOf;
      } else {
      post({ type: 'status', text: 'ChatGPT · listing' });
      // A quick check reads the newest page or two of the main list and nothing else; archived
      // chats and projects are walked only in the daily full list. Every request counts against
      // the same hourly allowance as the person's own use of ChatGPT.
      for (const archived of (partial ? [false] : [false, true])) {
        for (let offset = 0; ; offset += 100) {
          const page = await getJson('/backend-api/conversations?offset=' + offset + '&limit=100&order=updated&is_archived=' + archived, { headers });
          const list = page.items || [];
          for (const c of list) items.push({ id: 'chatgpt:' + c.id, updated: toIso(c.update_time), created: toIso(c.create_time), title: c.title || '' });
          post({ type: 'status', text: partial ? 'ChatGPT · checking for new chats' : 'ChatGPT · listing all chats (once a day) · ' + items.length + ' so far' });
          if (list.length < 100 || offset > 50000 || (partial && (older(list) || offset >= 400))) break;
          await sleep(2000);
        }
      }
      // Conversations inside projects are listed per project.
      if (!partial) try {
        const sidebar = await getJson('/backend-api/gizmos/snorlax/sidebar?conversations_per_gizmo=0', { headers });
        for (const entry of sidebar.items || []) {
          const g = (entry.gizmo && entry.gizmo.gizmo) || entry.gizmo || {};
          if (!g.id) continue;
          const name = (g.display && g.display.name) || '';
          let cursor = '0';
          const cursors = new Set();
          // Stop when the cursor runs out, repeats, or a page brings nothing.
          while (cursor !== null && cursor !== undefined && cursor !== '' && !cursors.has(String(cursor)) && cursors.size < 200) {
            cursors.add(String(cursor));
            const page = await getJson('/backend-api/gizmos/' + g.id + '/conversations?cursor=' + encodeURIComponent(cursor), { headers });
            const list = page.items || [];
            for (const c of list) {
              items.push({ id: 'chatgpt:' + c.id, updated: toIso(c.update_time), created: toIso(c.create_time), title: c.title || '', project: name });
              projectOf[c.id] = name;
            }
            post({ type: 'status', text: partial ? 'ChatGPT · checking for new chats' : 'ChatGPT · listing all chats (once a day) · ' + items.length + ' so far' });
            if (!list.length) break;
            cursor = page.cursor;
            await sleep(2000);
          }
        }
      } catch (e) { if (e.signedOut || e.rateLimited) throw e; post({ type: 'error', text: 'ChatGPT projects: ' + e.message }); }
      try { sessionStorage.setItem(cacheKey, JSON.stringify({ at: Date.now(), items, projectOf, full: !partial })); } catch (e) {}
      }

)JS" LR"JS(
      { const seen = new Set(); items = items.reverse().filter((x) => !seen.has(x.id) && seen.add(x.id)).reverse(); }
      const ids = await askWhichToFetch({ type: 'list', account, accountName: acct.name, complete: !partial, items });
      let n = 0;
      for (const full of ids) {
        const id = full.replace(/^chatgpt:/, '');
        try {
          const c = await getJson('/backend-api/conversation/' + id, { headers }, true);
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
            // Only what was shown in the chat: not calls to tools (searches, file reads,
            // connector paths), code run, or the "I'll check..." notes between them.
            .filter((m) => !m.recipient || m.recipient === 'all')
            .filter((m) => !m.channel || m.channel === 'final')
            .filter((m) => { const t = (m.content && m.content.content_type) || 'text'; return t === 'text' || t === 'multimodal_text'; })
            .map((m) => ({ role: m.author.role, ts: toIso(m.create_time), text: textOf(m) }))
            .filter((m) => m.text && m.text.trim());
          post({ type: 'conversation', id: full, account, title: c.title || '', project: projectOf[id] || '',
                 created: toIso(c.create_time), updated: toIso(c.update_time), messages });
        } catch (e) {
          if (e.signedOut || e.rateLimited) throw e;
          post({ type: 'error', text: 'ChatGPT conversation ' + id + ': ' + e.message });
        }
        if (++n % 10 === 0) post({ type: 'status', text: 'ChatGPT · ' + n + ' of ' + ids.length });
        // ChatGPT rate-limits conversation reads: one every 5 s normally, slower for a while
        // after it has asked Sessions to slow down (10, 20, 40 s ... up to 5 minutes).
        for (let gap = throttle ? Math.min(5000 * Math.pow(2, throttle), 300000) : 5000; gap > 0; gap -= 30000) {
          if (gap > 30000) post({ type: 'status', text: 'ChatGPT · ' + n + ' of ' + ids.length + ' · going slowly; ChatGPT asked for fewer requests' });
          await sleep(Math.min(gap, 30000));
        }
        if (throttle && n % 20 === 0) throttle--;
      }
    }
    post({ type: 'done' });
  } catch (e) {
    post(e.rateLimited ? { type: 'rateLimited', seconds: e.seconds || 0 } : e.signedOut ? { type: 'signedOut', detail: e.message } : { type: 'error', text: 'ChatGPT: ' + e.message, fatal: true });
  }
})();
)JS";

// Gemini has no JSON API; the web app talks to "batchexecute" with
// positional arrays. MaZiqc lists chats, hNvQHb reads one.
constexpr wchar_t Gemini[] = LR"JS(
(async () => {
  try {
    const W = window.WIZ_global_data || {};
    if (!W.SNlM0e) { post({ type: 'signedOut', detail: 'no session token on the Gemini page' }); return; }
    let reqid = Math.floor(Math.random() * 90000) + 10000;
    // Each Google account signed in to the browser has its own path: /u/0, /u/1, ...
    const prefix = (location.pathname.match(/^\/u\/\d+/) || [''])[0];
    async function rpc(id, payload) {
      reqid += 100000;
      const body = new URLSearchParams({ 'f.req': JSON.stringify([[[id, JSON.stringify(payload), null, 'generic']]]), at: W.SNlM0e });
      const url = prefix + '/_/BardChatUi/data/batchexecute?rpcids=' + id + '&source-path=' + encodeURIComponent(prefix + '/app') +
                  '&bl=' + encodeURIComponent(W.cfb2h || '') +
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
    post({ type: 'identity', who: W.oPEP7c || '' });
    post({ type: 'status', text: 'Gemini · listing' });
    const items = [];
    const titles = {};
    let cursor = null;
    for (let page = 0; page < 200; page++) {
      const res = await rpc('MaZiqc', [13, cursor, [0, null, 1]]);
      const chats = (res && Array.isArray(res[2]) ? res[2] : []).filter((c) => Array.isArray(c) && typeof c[0] === 'string');
      for (const c of chats) {
        const updated = seconds(c[5]) || seconds(c[4]) || '';
        items.push({ id: 'gemini:' + c[0], updated, created: updated, title: c[1] || '' });
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
        if (e.signedOut || e.rateLimited) throw e;
        post({ type: 'error', text: 'Gemini conversation ' + id + ': ' + e.message });
      }
      if (++n % 10 === 0) post({ type: 'status', text: 'Gemini · ' + n + ' of ' + ids.length });
      await sleep(400);
    }
    post({ type: 'done' });
  } catch (e) {
    post(e.rateLimited ? { type: 'rateLimited', seconds: e.seconds || 0 } : e.signedOut ? { type: 'signedOut', detail: e.message } : { type: 'error', text: 'Gemini: ' + e.message, fatal: true });
  }
})();
)JS";

}  // namespace sessions::agents
