#!/usr/bin/env node

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { fileURLToPath } from "node:url";
import crypto from "node:crypto";
import http from "node:http";
import https from "node:https";
import url from "node:url";
import { spawn } from "node:child_process";

const logger = {
  info: (msg) => console.error(`[${new Date().toISOString()}] INFO (navidrome): ${msg}`),
  warn: (msg) => console.error(`[${new Date().toISOString()}] WARN (navidrome): ${msg}`),
  error: (msg) => console.error(`[${new Date().toISOString()}] ERROR (navidrome): ${msg}`),
  debug: (msg) => console.error(`[${new Date().toISOString()}] DEBUG (navidrome): ${msg}`),
};

logger.info("Script started (v4.2-compat-final).");

const NAVIDROME_URL = process.env.NAVIDROME_URL || "http://xx.xxx.xxx:4533";
const NAVIDROME_USER = process.env.NAVIDROME_USER || "UserName";
const NAVIDROME_PASS = process.env.NAVIDROME_PASS || "Password";
const CLIENT_NAME = "XiaoZhi-MCP-Tool";
const API_VERSION = "1.16.1";
const HTTP_BRIDGE_PORT = Number(process.env.HTTP_BRIDGE_PORT || 8088);
const PROXY_SERVER_IP = process.env.PROXY_SERVER_IP || "xxx.xxx.xxx.xxx(代理服务器的地址-此脚本，代理端口为8080)";

async function ensureFetch() {
  if (globalThis.fetch) return globalThis.fetch;
  const mod = await import("node-fetch");
  return mod.default || mod;
}

// ----------- NavidromeClient --------------
class NavidromeClient {
  constructor() {
    this.salt = crypto.randomBytes(6).toString("hex");
    this.token = crypto.createHash("md5").update(NAVIDROME_PASS + this.salt).digest("hex");
    this.apiAuthParams = new URLSearchParams({ u: NAVIDROME_USER, t: this.token, s: this.salt, v: API_VERSION, c: CLIENT_NAME, f: "json" }).toString();
    this.streamAuthParams = new URLSearchParams({ u: NAVIDROME_USER, t: this.token, s: this.salt, v: API_VERSION, c: CLIENT_NAME }).toString();
    this.isInitialized = false;
    this.ndToken = null;
    this.ndClientId = null;
    this.loggedIn = false;
  }

  async #request(endpoint, params = {}) {
    const fetch = await ensureFetch();
    const extra = new URLSearchParams(params).toString();
    const urlStr = `${NAVIDROME_URL.replace(/\/$/, "")}/rest/${endpoint}?${this.apiAuthParams}${extra ? `&${extra}` : ""}`;
    logger.debug(`Navidrome API request: ${urlStr}`);
    const res = await fetch(urlStr, { method: "GET" });
    if (!res.ok) { const txt = await res.text().catch(() => ""); throw new Error(`HTTP ${res.status} ${res.statusText} - ${txt}`); }
    const j = await res.json().catch(() => null);
    if (!j || !j["subsonic-response"]) throw new Error("Invalid Navidrome response");
    const body = j["subsonic-response"];
    if (body.status === "failed") { const e = body.error || {}; throw new Error(`Navidrome API error code=${e.code} msg=${e.message}`); }
    return body;
  }

  async apiRequest(path, params = {}) {
    const fetch = await ensureFetch();
    const base = `${NAVIDROME_URL.replace(/\/$/, "")}/api/${path.replace(/^\/+/, "")}`;
    const u = new URL(base);
    Object.entries(params || {}).forEach(([k, v]) => u.searchParams.append(k, v));
    const headers = {};
    if (this.ndToken) headers['x-nd-authorization'] = `Bearer ${this.ndToken}`;
    if (this.ndClientId) headers['x-nd-client-unique-id'] = this.ndClientId;
    logger.debug(`Navidrome native API request: ${u.toString()} headers=${Object.keys(headers).join(',')}`);
    const res = await fetch(u.toString(), { method: "GET", headers });
    if (!res.ok) { const txt = await res.text().catch(()=> ""); throw new Error(`Native API HTTP ${res.status} ${res.statusText} - ${txt}`); }
    const j = await res.json().catch(()=>null);
    return j;
  }

  async loginNativeIfNeeded() {
    if (this.loggedIn) return;
    try {
      const fetch = await ensureFetch();
      const loginUrl = `${NAVIDROME_URL.replace(/\/$/, "")}/auth/login`;
      logger.debug(`Navidrome native login: ${loginUrl}`);
      const res = await fetch(loginUrl, {
        method: "POST",
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ username: NAVIDROME_USER, password: NAVIDROME_PASS })
      });
      if (!res.ok) { logger.debug(`Native login failed status=${res.status}`); return; }
      const j = await res.json().catch(()=>null);
      if (j && j.token && j.id) {
        this.ndToken = j.token;
        this.ndClientId = j.id;
        this.loggedIn = true;
        logger.info("Navidrome native login succeeded");
      } else {
        logger.debug("Navidrome native login returned no token/id");
      }
    } catch (err) {
      logger.debug("Navidrome native login error: " + (err && err.message));
    }
  }

  async ping() { if (this.isInitialized) return; await this.#request("ping"); this.isInitialized = true; }

  async search3({ query, count = 50, artistCount = 5, albumCount = 5 }) {
    const body = await this.#request("search3", { query, songCount: count, artistCount, albumCount });
    return {
      songs: body.searchResult3?.song ? (Array.isArray(body.searchResult3.song) ? body.searchResult3.song : [body.searchResult3.song]) : [],
      artists: body.searchResult3?.artist ? (Array.isArray(body.searchResult3.artist) ? body.searchResult3.artist : [body.searchResult3.artist]) : [],
      albums: body.searchResult3?.album ? (Array.isArray(body.searchResult3.album) ? body.searchResult3.album : [body.searchResult3.album]) : [],
    };
  }

  async getRandomSongs({ size = 50 } = {}) { const body = await this.#request("getRandomSongs", { size }); const songs = body.randomSongs?.song; return songs ? (Array.isArray(songs) ? songs : [songs]) : []; }
  async getPlaylists() { const body = await this.#request("getPlaylists"); const playlists = body.playlists?.playlist; return playlists ? (Array.isArray(playlists) ? playlists : [playlists]) : []; }
  async getPlaylistSongs({ playlistId }) { if (!playlistId) throw new Error("playlistId required"); const body = await this.#request("getPlaylist", { id: playlistId }); const songs = body.playlist?.entry || body.playlist?.song; return songs ? (Array.isArray(songs) ? songs : [songs]) : []; }

  async getAlbum({ id }) {
    const body = await this.#request("getAlbum", { id });
    const songs = body.album?.song;
    return songs ? (Array.isArray(songs) ? songs : [songs]) : [];
  }

  async getArtist({ id }) {
      const body = await this.#request("getArtist", { id });
      const albums = body.artist?.album;
      let allSongs = [];
      if (albums) {
          for (const album of albums) {
              const songs = await this.getAlbum({ id: album.id });
              allSongs = allSongs.concat(songs);
          }
      }
      return allSongs;
  }

  _isMeaningfulEntry(e) {
    if (!e) return false;
    if (Array.isArray(e.line) && e.line.length > 0) {
      for (const ln of e.line) {
        const txt = (ln.value ?? ln.text ?? ln.lyric ?? "").toString().trim();
        if (txt.length > 0) return true;
      }
      return false;
    }
    if (Array.isArray(e.lines) && e.lines.length > 0) {
      for (const ln of e.lines) {
        const txt = (ln.value ?? ln.text ?? ln.lyric ?? "").toString().trim();
        if (txt.length > 0) return true;
      }
      return false;
    }
    const s = (e.lyrics ?? e.value ?? e.text ?? "").toString().trim();
    return s.length > 0;
  }

  async getLyrics({ id }) {
    try {
      const body = await this.#request("getLyrics", { id });
      logger.debug(`[Navidrome] getLyrics raw body: ${JSON.stringify(body).slice(0,2000)}`);
      let lyrics = body.lyrics || body.song?.lyrics;
      if (lyrics) {
        const arr = Array.isArray(lyrics) ? lyrics : [lyrics];
        const meaningful = arr.filter(e => this._isMeaningfulEntry(e));
        if (meaningful.length > 0) {
          return meaningful;
        }
        logger.debug(`[Navidrome] getLyrics returned entries but none are meaningful for id=${id}`);
      }
    } catch (err) {
      logger.debug(`getLyrics failed or not supported: ${err && err.message}`);
    }

    try {
      const body2 = await this.#request("getSong", { id });
      logger.debug(`[Navidrome] getSong raw body: ${JSON.stringify(body2).slice(0,2000)}`);
      const fallback = body2.song?.lyrics;
      if (fallback) {
        const arr2 = Array.isArray(fallback) ? fallback : [fallback];
        const meaningful2 = arr2.filter(e => this._isMeaningfulEntry(e));
        if (meaningful2.length > 0) return meaningful2;
      }
    } catch (err) {
      logger.debug(`getSong fallback failed: ${err && err.message}`);
    }

    try {
      await this.loginNativeIfNeeded();
      if (this.loggedIn) {
        const body3 = await this.apiRequest(`song/${encodeURIComponent(id)}`);
        logger.debug(`[Navidrome native] /api/song/${id} raw: ${JSON.stringify(body3).slice(0,2000)}`);
        let nativeLyrics = (body3 && (body3.song?.lyrics ?? body3.lyrics));
        if (nativeLyrics) {
          try {
            if (typeof nativeLyrics === "string" && nativeLyrics.trim().startsWith("[")) {
              nativeLyrics = JSON.parse(nativeLyrics);
            }
          } catch (e) { /* ignore */ }
          const arr3 = Array.isArray(nativeLyrics) ? nativeLyrics : [nativeLyrics];
          const meaningful3 = arr3.filter(e => this._isMeaningfulEntry(e));
          if (meaningful3.length > 0) return meaningful3;
        }
      }
    } catch (err) {
      logger.debug(`native API get song failed: ${err && err.message}`);
    }

    return []; 
  }

  getStreamUrl(songId, options = {}) { if (!songId) throw new Error("songId required"); const params = new URLSearchParams({ id: String(songId) }); for (const [k, v] of Object.entries(options || {})) params.append(k, String(v)); return `${NAVIDROME_URL.replace(/\/$/, "")}/rest/stream?${this.streamAuthParams}&${params.toString()}`; }
}

let navidromeClient = null;

async function initializeNavidrome() {
  try {
    logger.info(`Connecting to Navidrome at ${NAVIDROME_URL}`);
    navidromeClient = new NavidromeClient();
    await navidromeClient.ping();
    logger.info("Navidrome connection OK");
    return true;
  } catch (err) {
    logger.error("Failed to initialize Navidrome client: " + (err.stack || err.message));
    navidromeClient = null;
    return false;
  }
}

const checkInitialized = () => {
  if (!navidromeClient?.isInitialized) {
    return { isError: true, content: [{ type: "text", text: "Navidrome服务尚未准备就绪，请稍后再试。" }] };
  }
  return null;
};

// ------------- NowPlaying Manager --------------
class NowPlayingManager {
    constructor() {
        this.queue = [];         // { id, title, artist, album, coverArt, raw }
        this.currentIndex = -1;
    }
    setQueue(songs, startPlaying = true) {
        if (!songs || songs.length === 0) { this.clear(); return null; }
        this.queue = songs.map(s => ({ id: s.id, title: s.title, artist: s.artist || s.displayArtist || (s.artists && s.artists.map(a=>a.name).join('/')) || "", album: s.album, coverArt: s.coverArt || s.coverArtId || s.cover, raw: s }));
        this.currentIndex = startPlaying ? 0 : -1;
        logger.info(`[Queue] Set new queue with ${this.queue.length} songs. Current index: ${this.currentIndex}`);
        return this.getCurrentSong();
    }
    toArray() { return this.queue.map(q => ({ ...q })); }
    shuffle() {
        if (this.queue.length < 2) return;
        for (let i = this.queue.length - 1; i > 0; i--) {
            const j = Math.floor(Math.random() * (i + 1));
            [this.queue[i], this.queue[j]] = [this.queue[j], this.queue[i]];
        }
        logger.info(`[Queue] Shuffled queue.`);
    }
    getCurrentSong() {
        if (this.currentIndex >= 0 && this.currentIndex < this.queue.length) {
            return this.queue[this.currentIndex];
        }
        return null;
    }
    next() {
        if (this.currentIndex < this.queue.length - 1) {
            this.currentIndex++;
            logger.info(`[Queue] Moved to next song. Index: ${this.currentIndex}`);
            return this.getCurrentSong();
        }
        logger.warn(`[Queue] Already at the end of the queue.`);
        return null;
    }
    previous() {
        if (this.currentIndex > 0) {
            this.currentIndex--;
            logger.info(`[Queue] Moved to previous song. Index: ${this.currentIndex}`);
            return this.getCurrentSong();
        }
        logger.warn(`[Queue] Already at the start of the queue.`);
        return null;
    }
    clear() {
        this.queue = [];
        this.currentIndex = -1;
        logger.info(`[Queue] Cleared.`);
    }
}
const nowPlaying = new NowPlayingManager();

// ------------ Proxy & Utils (同前) -------------
function proxyRequestToUpstream(upstreamUrl, clientReq, clientRes, redirectCount = 0, triedResume = false, retryCount = 0) {
  if (redirectCount > 5) {
    clientRes.writeHead(508, { "Content-Type": "text/plain" }).end("Loop Detected");
    return;
  }
  if (retryCount > 2) {
    logger.error(`[Proxy] Giving up after ${retryCount} retries for ${upstreamUrl}`);
    if (!clientRes.headersSent) clientRes.writeHead(502).end();
    else try { clientRes.end(); } catch(e) {}
    return;
  }

  const u = new URL(upstreamUrl);
  const httpMod = u.protocol === "https:" ? https : http;

  const headers = {
    'accept-encoding': 'identity',
    'connection': 'keep-alive',
    'user-agent': 'XiaoZhi-Proxy/1.0'
  };
  const forwardHeaders = ['range', 'if-range', 'authorization', 'cookie'];
  for (const h of forwardHeaders) {
    if (clientReq && clientReq.headers && clientReq.headers[h]) {
      headers[h] = clientReq.headers[h];
    }
  }

  const isTranscode = (u.search && (u.search.includes('transcode=') || u.search.includes('format=mp3') || u.search.includes('format=mp3'.toLowerCase())));
  if (isTranscode && headers['range']) {
    delete headers['range'];
    delete headers['if-range'];
    logger.info(`[Proxy] Detected transcode request; removed client Range headers to avoid upstream raw-chunk responses.`);
  }

  logger.debug(`[Proxy] Upstream request: GET ${u.toString()} headers=${Object.keys(headers).join(',')}`);

  const options = {
    hostname: u.hostname,
    port: u.port || (u.protocol === "https:" ? 443 : 80),
    path: u.pathname + u.search,
    method: 'GET',
    timeout: 120000,
    headers
  };

  const upstreamReq = httpMod.request(options, (upstreamRes) => {
    if (upstreamRes.statusCode >= 300 && upstreamRes.statusCode < 400 && upstreamRes.headers.location) {
      const nextUrl = new URL(upstreamRes.headers.location, upstreamUrl).toString();
      logger.info(`[Proxy] Following redirect to: ${nextUrl}`);
      upstreamRes.resume();
      proxyRequestToUpstream(nextUrl, clientReq, clientRes, redirectCount + 1, false, 0);
      return;
    }

    const upstreamCL = upstreamRes.headers['content-length'] ? Number(upstreamRes.headers['content-length']) : null;
    if (upstreamRes.statusCode === 206 && upstreamCL !== null && upstreamCL <= 2 && clientReq && clientReq.headers && clientReq.headers.range && retryCount < 2) {
      logger.warn(`[Proxy] Upstream returned tiny 206 (probe) content-length=${upstreamCL}. Retrying without Range.`);
      upstreamRes.resume();
      if (options.headers) delete options.headers['range'];
      proxyRequestToUpstream(upstreamUrl, clientReq, clientRes, redirectCount, triedResume, retryCount + 1);
      return;
    }

    if (upstreamRes.statusCode !== 200 && upstreamRes.statusCode !== 206) {
      logger.error(`[Proxy] Upstream server returned status: ${upstreamRes.statusCode}. Forwarding status.`);
      const hopByHop = new Set(['connection','keep-alive','proxy-authenticate','proxy-authorization','te','trailers','transfer-encoding','upgrade']);
      const outHeaders = {};
      Object.entries(upstreamRes.headers).forEach(([k,v]) => {
        if (!hopByHop.has(k.toLowerCase())) outHeaders[k] = v;
      });
      delete outHeaders['content-length'];
      outHeaders['transfer-encoding'] = 'chunked';
      if (isTranscode) outHeaders['accept-ranges'] = 'none';
      clientRes.writeHead(upstreamRes.statusCode, outHeaders);
      upstreamRes.pipe(clientRes);
      return;
    }

    const hopByHop = new Set(['connection','keep-alive','proxy-authenticate','proxy-authorization','te','trailers','transfer-encoding','upgrade']);
    const outHeaders = {};
    Object.entries(upstreamRes.headers).forEach(([k,v]) => {
      if (!hopByHop.has(k.toLowerCase())) outHeaders[k] = v;
    });
    delete outHeaders['content-length'];
    outHeaders['transfer-encoding'] = 'chunked';
    if (isTranscode) {
      outHeaders['accept-ranges'] = 'none';
    } else {
      if (upstreamRes.headers['accept-ranges']) outHeaders['accept-ranges'] = upstreamRes.headers['accept-ranges'];
    }
    if (!outHeaders['content-type']) outHeaders['content-type'] = upstreamRes.headers['content-type'] || 'application/octet-stream';

    const upstreamContentLength = upstreamRes.headers['content-length'] ? Number(upstreamRes.headers['content-length']) : null;
    const contentRange = upstreamRes.headers['content-range'] || null;
    logger.info(`[Proxy] Forwarding upstream ${upstreamRes.statusCode} to client; content-type=${upstreamRes.headers['content-type']}, upstreamCL=${upstreamContentLength}, content-range=${contentRange}`);

    clientRes.writeHead(200, outHeaders);

    let totalBytes = 0;
    let ended = false;
    let clientClosed = false;
    clientRes.on('close', () => { clientClosed = true; });

    upstreamRes.on('data', (chunk) => {
      totalBytes += chunk.length;
      const ok = clientRes.write(chunk);
      if (!ok) {
        upstreamRes.pause();
        clientRes.once('drain', () => upstreamRes.resume());
      }
    });

    const tryResumeIfNeeded = () => {
      if (isTranscode) return false;
      if (upstreamContentLength && totalBytes < upstreamContentLength && !triedResume && !clientClosed) {
        logger.warn(`[Proxy] Upstream closed early (received ${totalBytes} < claimed ${upstreamContentLength}). Trying resume from ${totalBytes}`);
        const resumeHeaders = Object.assign({}, headers);
        resumeHeaders['range'] = `bytes=${totalBytes}-`;
        const resumeOptions = {
          hostname: u.hostname,
          port: u.port || (u.protocol === "https:" ? 443 : 80),
          path: u.pathname + u.search,
          method: 'GET',
          timeout: 120000,
          headers: resumeHeaders
        };
        try {
          const resumeReq = httpMod.request(resumeOptions, (resumeRes) => {
            if (resumeRes.statusCode === 206 || resumeRes.statusCode === 200) {
              logger.info(`[Proxy] Resume request status ${resumeRes.statusCode}, piping remainder...`);
              resumeRes.on('data', (c) => {
                totalBytes += c.length;
                const ok2 = clientRes.write(c);
                if (!ok2) {
                  resumeRes.pause();
                  clientRes.once('drain', () => resumeRes.resume());
                }
              });
              resumeRes.on('end', () => {
                logger.info(`[Proxy] Resume completed; final totalBytes=${totalBytes}`);
                try { clientRes.end(); } catch(e) {}
              });
              resumeRes.on('error', (e) => {
                logger.error(`[Proxy] Resume stream error: ${e && e.message}`);
                try { clientRes.end(); } catch(e) {}
              });
            } else {
              logger.warn(`[Proxy] Resume request returned status ${resumeRes.statusCode}; cannot resume.`);
              try { clientRes.end(); } catch(e) {}
            }
          });
          resumeReq.on('error', (e) => {
            logger.error(`[Proxy] Resume request error: ${e && e.message}`);
            try { clientRes.end(); } catch(e) {}
          });
          resumeReq.on('timeout', () => { logger.error('[Proxy] Resume request timed out'); resumeReq.destroy(); });
          resumeReq.end();
        } catch (e) {
          logger.error(`[Proxy] Resume flow failed: ${e && e.message}`);
          try { clientRes.end(); } catch(e) {}
        }
        return true;
      }
      return false;
    };

    upstreamRes.on('end', () => {
      ended = true;
      logger.info(`[Proxy] Upstream 'end' for ${upstreamUrl}. totalBytesReceived=${totalBytes}`);
      if (!tryResumeIfNeeded()) {
        try { clientRes.end(); } catch(e) {}
      }
    });

    upstreamRes.on('close', () => {
      if (!ended) {
        logger.warn(`[Proxy] Upstream connection closed unexpectedly for ${upstreamUrl}. totalBytes=${totalBytes}`);
        if (!triedResume && !clientClosed) {
          const resumed = tryResumeIfNeeded();
          if (!resumed) {
            try { clientRes.end(); } catch(e) {}
          }
        } else {
          try { clientRes.end(); } catch(e) {}
        }
      }
    });

    upstreamRes.on('error', (e) => {
      logger.error(`[Proxy] upstreamRes error: ${e && e.message}`);
      if (!triedResume && !clientClosed && tryResumeIfNeeded()) {
      } else {
        try { clientRes.end(); } catch(ex) {}
      }
    });
  });

  upstreamReq.on('error', (e) => {
    logger.error(`[Proxy] Upstream request error: ${e && e.message}`);
    if (retryCount < 2) {
      logger.info(`[Proxy] Retrying upstream request without Range (retry ${retryCount+1})`);
      if (options.headers) delete options.headers['range'];
      proxyRequestToUpstream(upstreamUrl, clientReq, clientRes, redirectCount, triedResume, retryCount + 1);
    } else {
      if (!clientRes.headersSent) clientRes.writeHead(502).end();
      else try { clientRes.end(); } catch(e) {}
    }
  });

  upstreamReq.on('timeout', () => {
    logger.error('[Proxy] Upstream request timed out.');
    upstreamReq.destroy();
  });

  upstreamReq.end();
}

function formatLyricsToLrc(lyricsArray) {
  if (!lyricsArray || lyricsArray.length === 0) return "";
  function pickEntry(arr) {
    for (const e of arr) if (e && e.synced && Array.isArray(e.line) && e.line.length > 0) return e;
    for (const e of arr) if (e && Array.isArray(e.line) && e.line.length > 0) return e;
    for (const e of arr) if (e && typeof e.lyrics === "string" && e.lyrics.trim().length > 0) return e;
    for (const e of arr) if (e && typeof e.value === "string" && e.value.trim().length > 0) return e;
    return arr[0];
  }
  const chosen = pickEntry(lyricsArray);
  if (!chosen) return "";
  if (Array.isArray(chosen.line) && chosen.line.length > 0) {
    let lrc = "";
    for (const ln of chosen.line) {
      const start = Number(ln.start ?? ln.time ?? 0);
      const text = (ln.value ?? ln.text ?? ln.lyric ?? "").toString();
      const minutes = Math.floor(start / 60000);
      const seconds = Math.floor((start % 60000) / 1000);
      const ms = Math.floor((start % 1000) / 10);
      const tag = `[${minutes.toString().padStart(2,'0')}:${seconds.toString().padStart(2,'0')}.${ms.toString().padStart(2,'0')}]`;
      lrc += `${tag}${text}\n`;
    }
    return lrc;
  }
  if (Array.isArray(chosen.lines) && chosen.lines.length > 0) {
    let lrc = "";
    for (const ln of chosen.lines) {
      const start = Number(ln.start ?? ln.time ?? 0);
      const text = (ln.value ?? ln.text ?? ln.lyric ?? "").toString();
      const minutes = Math.floor(start / 60000);
      const seconds = Math.floor((start % 60000) / 1000);
      const ms = Math.floor((start % 1000) / 10);
      const tag = `[${minutes.toString().padStart(2,'0')}:${seconds.toString().padStart(2,'0')}.${ms.toString().padStart(2,'0')}]`;
      lrc += `${tag}${text}\n`;
    }
    return lrc;
  }
  if (typeof chosen.lyrics === "string" && chosen.lyrics.trim().length > 0) {
    return chosen.lyrics.split(/\r?\n/).map(s => s.trim()).filter(Boolean).join("\n");
  }
  if (typeof chosen.value === "string" && chosen.value.trim().length > 0) {
    return chosen.value.split(/\r?\n/).map(s => s.trim()).filter(Boolean).join("\n");
  }
  if (typeof chosen.text === "string" && chosen.text.trim().length > 0) {
    return chosen.text.split(/\r?\n/).map(s => s.trim()).filter(Boolean).join("\n");
  }
  return "";
}

function normalizeText(s) {
  if (!s) return "";
  let t = s.toString().trim().toLowerCase();
  t = t.replace(/[\uFF01-\uFF5E]/g, ch => String.fromCharCode(ch.charCodeAt(0) - 0xFEE0));
  t = t.replace(/[“”"'\(\)\[\]【】.,。?!，？！:：\-–—_/\\]/g, " ");
  t = t.replace(/\s+/g, " ").trim();
  return t;
}
function parseArtistTitleFromQuery(rawQuery) {
  if (!rawQuery) return { query: "" };
  let q = rawQuery;
  try { q = decodeURIComponent(q); } catch (e) { /* ignore */ }
  q = q.trim();

  const cleanupTitle = (title) => {
    if (!title) return "";
    return title.replace(/^(播放|的|专辑|歌曲|的歌)\s*/, '').trim();
  };

  let m = q.match(/^(.+?)的(.+)$/);
  if (m) {
    return { 
      artist: m[1].trim(), 
      title: cleanupTitle(m[2]), // 清理标题
      query: q 
    };
  }
  
  const sepCandidates = [' - ', ' — ', '-', '–', '／', '/', ':'];
  for (const sep of sepCandidates) {
    if (q.includes(sep)) {
      const parts = q.split(sep).map(s => s.trim()).filter(Boolean);
      if (parts.length >= 2) {
        return { 
          artist: parts[0], 
          title: cleanupTitle(parts.slice(1).join(' ')), // 清理标题
          query: q 
        };
      }
    }
  }
  
  const tokens = q.split(/\s+/).filter(Boolean);
  if (tokens.length >= 2) {
    return { 
      artist: tokens[0], 
      title: cleanupTitle(tokens.slice(1).join(' ')), // 清理标题
      query: q 
    };
  }

  return { 
    title: cleanupTitle(q), // 清理标题
    query: q 
  };
}

// ---------- HTTP Bridge (含 /control /now_playing /http_play) ------------
function startHttpBridge() {
  const server = http.createServer(async (req, res) => {
    const parsed = url.parse(req.url, true);
    const pathname = parsed.pathname || "";
    logger.info(`[HTTP Bridge] ${req.method} ${req.url}`);

    // 主入口：stream_pcm（保留兼容逻辑）
    if (pathname === "/stream_pcm") {
      const query = (parsed.query.song || "").trim();
      if (!query || !navidromeClient) {
        const statusCode = !query ? 400 : 503;
        const errorMsg = !query ? "missing song parameter" : "Navidrome client not ready";
        res.writeHead(statusCode, { "Content-Type": "application/json" }).end(JSON.stringify({ error: errorMsg }));
        return;
      }
      try {
        // 以下逻辑与之前版本一致：支持 artist 的所有歌、专辑、歌单、单曲、随机
        const parsedQT = parseArtistTitleFromQuery(query);
		const rawLower = query.toLowerCase();
		let wantArtistAll = false;

		if (parsedQT.artist && parsedQT.title) {
		  const t = parsedQT.title.toLowerCase();
		  if (/^歌|歌曲|全部|所有|所有歌曲|全部歌曲|的歌|的歌曲/.test(t) || rawLower.endsWith("的歌") || rawLower.endsWith("的歌曲")) {
			wantArtistAll = true;
		  }
		} else if (parsedQT.artist && !parsedQT.title) { // 用户只说了 "播放刘德华的歌"
			wantArtistAll = true;
		} else {
		  if (/的歌|的歌曲|全部歌曲|所有歌曲/.test(rawLower)) {
			const m = rawLower.match(/^(.+?)的(歌曲|歌|全部|所有)/);
			if (m) {
			  parsedQT.artist = (m[1] || "").trim();
			  parsedQT.title = m[2];
			  wantArtistAll = true;
			}
		  }
		}

		let wantAlbum = false;
		// [新功能] 改进专辑意图检测
		if (!wantArtistAll && (rawLower.includes('专辑') || (parsedQT.artist && parsedQT.title))) {
			// 如果明确说了“专辑”，或者提供了艺术家和标题（这通常意味着是专辑或单曲）
			wantAlbum = true;
			// 从标题中移除“专辑”这个词
			if (parsedQT.title) {
				parsedQT.title = parsedQT.title.replace(/专辑/g, '').trim();
			}
		}

        let songToPlay = null;

        // ARTIST_ALL: 收集 artist 所有歌曲（getArtist + search3 合并）
        if (wantArtistAll) {
          logger.info(`[HTTP Bridge] Detected ARTIST_ALL intent for artist="${parsedQT.artist}" query="${query}"`);
          let artistId = null;
          try {
            const sr = await navidromeClient.search3({ query: parsedQT.artist, artistCount: 5, songCount: 0, albumCount: 0 });
            if (sr && Array.isArray(sr.artists) && sr.artists.length > 0) {
              artistId = sr.artists[0].id;
              logger.debug(`[HTTP Bridge] Found artistId=${artistId} for "${parsedQT.artist}"`);
            }
          } catch (e) {
            logger.debug(`[HTTP Bridge] artist search failed: ${e && e.message}`);
          }

          let allSongs = [];
          if (artistId) {
            try {
              const artistSongs = await navidromeClient.getArtist({ id: artistId });
              if (artistSongs && artistSongs.length > 0) {
                allSongs = allSongs.concat(artistSongs);
                logger.debug(`[HTTP Bridge] getArtist returned ${artistSongs.length} songs`);
              } else {
                logger.debug(`[HTTP Bridge] getArtist returned no songs for id=${artistId}`);
              }
            } catch (e) {
              logger.debug(`[HTTP Bridge] getArtist failed for id=${artistId}: ${e && e.message}`);
            }
          }

          try {
            const srSongs = await navidromeClient.search3({ query: parsedQT.artist, count: 500, artistCount: 0, albumCount: 0 });
            if (srSongs && Array.isArray(srSongs.songs) && srSongs.songs.length > 0) {
              logger.debug(`[HTTP Bridge] search3 for artist returned ${srSongs.songs.length} songs`);
              allSongs = allSongs.concat(srSongs.songs);
            }
          } catch (e) {
            logger.debug(`[HTTP Bridge] search3 (artist songs) failed: ${e && e.message}`);
          }

          const map = new Map();
          for (const s of allSongs) {
            if (!s || !s.id) continue;
            if (!map.has(s.id)) map.set(s.id, s);
          }
          const merged = Array.from(map.values());
          logger.info(`[HTTP Bridge] Collected ${merged.length} unique songs for artist="${parsedQT.artist}"`);

          if (!merged || merged.length === 0) {
            logger.warn(`[HTTP Bridge] No songs found for artist="${parsedQT.artist}" (after merge)`);
            const randomSongs = await navidromeClient.getRandomSongs({ size: 1 });
            if (randomSongs && randomSongs.length > 0) songToPlay = randomSongs[0];
            else throw new Error(`找不到艺术家或歌曲: ${parsedQT.artist}`);
          } else {
            nowPlaying.setQueue(merged, false);
            nowPlaying.shuffle();
            const firstSong = nowPlaying.next();
            if (!firstSong) throw new Error("队列为空，无法开始播放。");

            const tracks = nowPlaying.toArray().map(s => ({
              id: s.id,
              title: s.title,
              artist: s.artist,
              album: s.album,
              audio_url: `http://${PROXY_SERVER_IP}:${HTTP_BRIDGE_PORT}/stream_proxy?songId=${encodeURIComponent(s.id)}&force_cbr=1`,
              lyric_url: `http://${PROXY_SERVER_IP}:${HTTP_BRIDGE_PORT}/lyric_proxy?songId=${encodeURIComponent(s.id)}`
            }));

            const lyricCandidates = await navidromeClient.getLyrics({ id: firstSong.id }).catch(()=>[]);
            const lyricUrl = (Array.isArray(lyricCandidates) && lyricCandidates.length > 0) ? `http://${PROXY_SERVER_IP}:${HTTP_BRIDGE_PORT}/lyric_proxy?songId=${encodeURIComponent(firstSong.id)}` : "";

            const payload = {
              artist: parsedQT.artist,
              title: `${parsedQT.artist} 的歌曲（共 ${tracks.length} 首，已随机）`,
              audio_url: tracks[0].audio_url,
              lyric_url: lyricUrl,
              playlist: tracks,
              playlist_type: "artist_temp",
              random: true,
              current_index: 0
            };
            logger.info(`[HTTP Bridge] Responding ARTIST_PLAYLIST for "${parsedQT.artist}" count=${tracks.length}`);
            res.writeHead(200, { "Content-Type": "application/json" }).end(JSON.stringify(payload));
            return;
          }
        } // end artist_all

        // ALBUM: 专辑按顺序播放
        if (wantAlbum) {
          logger.info(`[HTTP Bridge] Detected ALBUM intent for artist="${parsedQT.artist}" album="${parsedQT.title}"`);
          let albumsFound = [];
          try {
            const q = parsedQT.artist ? `${parsedQT.artist} ${parsedQT.title}` : parsedQT.title;
            const sr = await navidromeClient.search3({ query: q, albumCount: 5, songCount: 0, artistCount: 0 });
            albumsFound = sr.albums || [];
          } catch (e) {
            logger.debug(`[HTTP Bridge] album search failed: ${e && e.message}`);
          }

          if (!albumsFound || albumsFound.length === 0) {
            logger.warn(`[HTTP Bridge] No album found for "${parsedQT.title}", fallback to song search`);
            const sr = await navidromeClient.search3({ query: parsedQT.title, count: 20 });
            if (sr && sr.songs && sr.songs.length > 0) {
              songToPlay = sr.songs[0];
            } else {
              res.writeHead(404, { "Content-Type": "application/json" }).end(JSON.stringify({ error: `找不到专辑或歌曲: ${parsedQT.title}` }));
              return;
            }
          } else {
            const targetAlbum = albumsFound[0];
            const albumSongs = await navidromeClient.getAlbum({ id: targetAlbum.id });
            if (!albumSongs || albumSongs.length === 0) {
              res.writeHead(404, { "Content-Type": "application/json" }).end(JSON.stringify({ error: `专辑 ${targetAlbum.name} 无歌曲` }));
              return;
            }
            nowPlaying.setQueue(albumSongs, false);
            // 专辑按原顺序播放（不 shuffle）
            const firstSong = nowPlaying.next();
            if (!firstSong) throw new Error("队列为空，无法开始播放。");

            const tracks = nowPlaying.toArray().map(s => ({
              id: s.id,
              title: s.title,
              artist: s.artist,
              album: s.album,
              audio_url: `http://${PROXY_SERVER_IP}:${HTTP_BRIDGE_PORT}/stream_proxy?songId=${encodeURIComponent(s.id)}&force_cbr=1`,
              lyric_url: `http://${PROXY_SERVER_IP}:${HTTP_BRIDGE_PORT}/lyric_proxy?songId=${encodeURIComponent(s.id)}`
            }));

            const lyricCandidates = await navidromeClient.getLyrics({ id: firstSong.id }).catch(()=>[]);
            const lyricUrl = (Array.isArray(lyricCandidates) && lyricCandidates.length > 0) ? `http://${PROXY_SERVER_IP}:${HTTP_BRIDGE_PORT}/lyric_proxy?songId=${encodeURIComponent(firstSong.id)}` : "";

            const payload = {
              artist: parsedQT.artist || firstSong.artist,
              title: `${targetAlbum.name}（专辑，共 ${tracks.length} 首）`,
              audio_url: tracks[0].audio_url,
              lyric_url: lyricUrl,
              playlist: tracks,
              playlist_type: "album",
              random: false,
              current_index: 0
            };
            logger.info(`[HTTP Bridge] Responding ALBUM_PLAYLIST for album="${targetAlbum.name}" count=${tracks.length}`);
            res.writeHead(200, { "Content-Type": "application/json" }).end(JSON.stringify(payload));
            return;
          }
        } // end album

        // SMART SEARCH -> 单曲
        logger.info(`[HTTP Bridge] Handling SMART SEARCH request for "${query}"`);

        const parsed2 = parseArtistTitleFromQuery(query);
        const normParsedArtist = normalizeText(parsed2.artist || "");
        const normParsedTitle = normalizeText(parsed2.title || "");
        const normRawQuery = normalizeText(parsed2.query || query);

        const searchQueries = [];
        if (parsed2.artist && parsed2.title) {
            searchQueries.push(`${parsed2.artist} ${parsed2.title}`);
            searchQueries.push(`${parsed2.title} ${parsed2.artist}`);
        }
        searchQueries.push(parsed2.query);
        if (parsed2.title && parsed2.title !== parsed2.query) searchQueries.push(parsed2.title);
        if (parsed2.artist && parsed2.artist !== parsed2.query) searchQueries.push(parsed2.artist);

        const candidatesMap = new Map();
        for (const sq of searchQueries) {
            if (!sq || sq.trim().length === 0) continue;
            try {
                const sr = await navidromeClient.search3({ query: sq, count: 20 });
                if (sr && Array.isArray(sr.songs)) {
                    for (const s of sr.songs) {
                        if (!s || !s.id) continue;
                        if (!candidatesMap.has(s.id)) candidatesMap.set(s.id, s);
                    }
                }
            } catch (err) {
                logger.debug(`[HTTP Bridge] search3 failed for "${sq}": ${err && err.message}`);
            }
        }

        let searchResultsSongs = Array.from(candidatesMap.values());
        if (!searchResultsSongs || searchResultsSongs.length === 0) {
            logger.warn(`[HTTP Bridge] No search3 candidates for "${query}", falling back to random`);
            const randomSongs = await navidromeClient.getRandomSongs({ size: 1 });
            if (randomSongs && randomSongs.length > 0) {
                songToPlay = randomSongs[0];
            } else {
                res.writeHead(404, { "Content-Type": "application/json" }).end(JSON.stringify({ error: `Song not found: ${query}` }));
                return;
            }
        } else {
            const scoreFor = (song) => {
                const sTitle = normalizeText(song.title || "");
                const sArtist = normalizeText(song.artist || "");
                let score = 0;
                if (normParsedTitle) {
                    if (sTitle === normalizeText(normParsedTitle)) score += 200;
                    else if (sTitle.includes(normalizeText(normParsedTitle))) score += 120;
                } else {
                    if (sTitle === normRawQuery) score += 150;
                    else if (sTitle.includes(normRawQuery)) score += 90;
                }
                if (normParsedArtist) {
                    if (sArtist === normalizeText(normParsedArtist)) score += 150;
                    else if (sArtist.includes(normalizeText(normParsedArtist))) score += 80;
                } else {
                    if (normRawQuery && sArtist.includes(normRawQuery)) score += 40;
                }
                if (normParsedTitle && sTitle.startsWith(normalizeText(normParsedTitle))) score += 20;
                if (!sTitle || sTitle.length < 1) score -= 50;
                return score;
            };

            let best = null;
            let bestScore = -Infinity;
            for (const s of searchResultsSongs) {
                try {
                    const sc = scoreFor(s);
                    logger.debug(`[HTTP Bridge] Candidate ${s.id} "${s.title}" by "${s.artist}" score=${sc}`);
                    if (sc > bestScore) { bestScore = sc; best = s; }
                } catch (e) {
                    logger.debug(`[HTTP Bridge] scoring error for ${s.id}: ${e && e.message}`);
                }
            }

            if (!best || bestScore < -10) {
                logger.warn(`[HTTP Bridge] Best candidate score low (${bestScore}) for "${query}", fallback to first search result`);
                best = searchResultsSongs[0];
            }

            songToPlay = best;
        }

        const streamProxyUrl = `http://${PROXY_SERVER_IP}:${HTTP_BRIDGE_PORT}/stream_proxy?songId=${encodeURIComponent(songToPlay.id)}&force_cbr=1`;

        let lyrics = [];
        try {
          lyrics = await navidromeClient.getLyrics({ id: songToPlay.id });
        } catch (err) {
          logger.debug(`[HTTP Bridge] getLyrics error for ${songToPlay.id}: ${err && err.message}`);
          lyrics = [];
        }

        const hasMeaningfulLyrics = Array.isArray(lyrics) && lyrics.length > 0;
        const lyricProxyUrl = hasMeaningfulLyrics ? `http://${PROXY_SERVER_IP}:${HTTP_BRIDGE_PORT}/lyric_proxy?songId=${encodeURIComponent(songToPlay.id)}` : "";

        const payload = {
          artist: songToPlay.artist || "未知艺术家", title: songToPlay.title || "未知歌曲",
          audio_url: streamProxyUrl
        };
        if (lyricProxyUrl) payload.lyric_url = lyricProxyUrl;

        logger.info(`[HTTP Bridge] Responding metadata for "${songToPlay.title}" -> proxy ${streamProxyUrl}, lyrics ${lyricProxyUrl || '(none)'}`);
        res.writeHead(200, { "Content-Type": "application/json" }).end(JSON.stringify(payload));
      } catch (err) {
        logger.error(`[HTTP Bridge] /stream_pcm error: ${err.stack || err.message}`);
        res.writeHead(500, { "Content-Type": "application/json" }).end(JSON.stringify({ error: "internal server error" }));
      }
      return;
    } // end /stream_pcm

	// stream_proxy: 代理音频，并根据参数决定是否使用FFmpeg强制转码
	if (pathname === "/stream_proxy") {
		  const songId = parsed.query.songId;
		  if (!songId || !navidromeClient) {
			res.writeHead(400, { "Content-Type": "application/json" }).end(JSON.stringify({ error: "missing songId or client not ready" }));
			return;
		  }

		  const forceCbr = parsed.query.force_cbr === '1';

		  if (!forceCbr) {
			// 非转码请求，逻辑保持不变
			logger.info(`[Proxy] Passthrough mode for songId=${songId}`);
			const transcodeOptions = {};
			for (const [k, v] of Object.entries(parsed.query || {})) {
			  if (k !== "songId" && k !== "force_cbr") transcodeOptions[k] = v;
			}
			const navidromeTranscodeUrl = navidromeClient.getStreamUrl(songId, transcodeOptions);
			proxyRequestToUpstream(navidromeTranscodeUrl, req, res);
			return;
		  }
		  
		  try {
			logger.info(`[FFmpeg-Buffered] Starting buffered CBR transcode for songId=${songId}`);

			res.writeHead(200, {
			  'Content-Type': 'audio/mpeg',
			  'Transfer-Encoding': 'chunked',
			  'Connection': 'keep-alive',
			  'Accept-Ranges': 'none'
			});

			const silentFrame = Buffer.from('fffb10c400000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000- '.repeat(242), 'hex');
			
			let sentRealData = false;
			const sendSilentFrames = () => {
			  if (sentRealData || res.writableEnded) return;
			  try {
				res.write(silentFrame);
			  } catch (e) {
				logger.warn(`[FFmpeg-Buffered] Error writing silent frame (client likely disconnected): ${e.message}`);
				return;
			  }
			  setTimeout(sendSilentFrames, 20);
			};
			sendSilentFrames();

			const ffmpegArgs = [
			  '-hide_banner', '-i', 'pipe:0', '-map', '0:a:0',
			  '-c:a', 'libmp3lame', '-ar', '44100', '-b:a', '192k',
			  '-ac', '2', '-f', 'mp3', '-flush_packets', '1',
			  'pipe:1'
			];
			const ffmpeg = spawn('/usr/bin/ffmpeg', ffmpegArgs, { stdio: ['pipe', 'pipe', 'pipe'] });
			
			const upstreamUrl = navidromeClient.getStreamUrl(songId);
			const u = new URL(upstreamUrl);
			const httpMod = u.protocol === "https:" ? https : http;
			const upstreamReq = httpMod.get(upstreamUrl, (upstreamRes) => {
			  if (upstreamRes.statusCode !== 200) {
				logger.error(`[FFmpeg-Buffered] Upstream returned status ${upstreamRes.statusCode}`);
				if (!ffmpeg.killed) ffmpeg.kill();
				if (!res.writableEnded) res.end();
				return;
			  }
			  logger.info(`[FFmpeg-Buffered] Connected to upstream. Piping to FFmpeg stdin.`);
			  upstreamRes.pipe(ffmpeg.stdin);
			});
			upstreamReq.on('error', (err) => logger.error(`[FFmpeg-Buffered] Upstream request error: ${err.message}`));

			// 为 ffmpeg.stdout 和 res 添加错误处理，防止 EPIPE 崩溃
			const cleanup = () => {
				if (!ffmpeg.killed) {
					ffmpeg.kill('SIGKILL');
				}
				if (upstreamReq) {
					upstreamReq.destroy();
				}
			};

			ffmpeg.stdout.on('error', (err) => {
				if (err.code === 'EPIPE') {
					logger.warn('[FFmpeg-Buffered] stdout pipe broken, client likely disconnected.');
				} else {
					logger.error(`[FFmpeg-Buffered] stdout error: ${err.message}`);
				}
				cleanup();
			});

			res.on('error', (err) => {
				if (err.code === 'EPIPE') {
					logger.warn('[FFmpeg-Buffered] Response stream pipe broken, client likely disconnected.');
				} else {
					logger.error(`[FFmpeg-Buffered] Response stream error: ${err.message}`);
				}
				cleanup();
			});
			
			ffmpeg.stdout.on('data', (chunk) => {
			  if (!sentRealData) {
				logger.info('[FFmpeg-Buffered] First chunk of real audio received. Stopping silent frames.');
				sentRealData = true;
			  }
			  if (!res.writableEnded) {
				// 在写入前检查连接是否还存在
				if (req.socket.destroyed) {
					logger.warn('[FFmpeg-Buffered] Client socket destroyed, stopping stream.');
					cleanup();
					return;
				}
				res.write(chunk);
			  }
			});

			ffmpeg.stderr.on('data', (data) => logger.debug(`[FFmpeg-Buffered stderr]: ${data.toString()}`));
			ffmpeg.on('close', (code) => {
			  logger.info(`[FFmpeg-Buffered] Process exited with code ${code}.`);
			  if (!res.writableEnded) res.end();
			});
			ffmpeg.on('error', (err) => {
			  logger.error(`[FFmpeg-Buffered] Process error: ${err.message}`);
			  if (!res.writableEnded) res.end();
			});
			req.on('close', () => {
			  logger.warn('[FFmpeg-Buffered] Client connection closed, initiating cleanup.');
			  cleanup();
			});

		  } catch (err) {
			logger.error(`[Proxy] Error in /stream_proxy: ${err.stack || err.message}`);
			if (!res.writableEnded) res.end();
		  }
		  return;
		}

    // lyric_proxy: 返回 LRC 格式歌词
    if (pathname === "/lyric_proxy") {
      const songId = parsed.query.songId;
      if (!songId || !navidromeClient) {
        res.writeHead(400, { "Content-Type": "application/json" }).end(JSON.stringify({ error: "missing songId or client not ready" }));
        return;
      }
      try {
        const lyrics = await navidromeClient.getLyrics({ id: songId });
        try {
          const raw = JSON.stringify(lyrics);
          logger.debug(`[Lyric Proxy] raw lyrics for ${songId}: ${raw.length > 2000 ? raw.slice(0,2000) + '... (truncated)' : raw}`);
        } catch (e) {
          logger.debug(`[Lyric Proxy] raw lyrics for ${songId}: <non-serializable>`);
        }

        const lrcText = formatLyricsToLrc(lyrics);
        logger.info(`[Lyric Proxy] formatted LRC size=${lrcText.length} for song ${songId}`);

        if (!lrcText || lrcText.trim().length === 0) {
          res.writeHead(204, { "Content-Type": "text/plain; charset=utf-8", "Cache-Control": "public, max-age=30" }).end();
          logger.info(`[Lyric Proxy] No lyrics available for song ${songId}`);
          return;
        }

        res.writeHead(200, { "Content-Type": "text/plain; charset=utf-8", "Cache-Control": "public, max-age=30" }).end(lrcText);
        logger.info(`[Lyric Proxy] Served formatted LRC for song ${songId}, length: ${lrcText.length}`);
      } catch (err) {
        logger.error(`[Lyric Proxy] Failed: ${err.stack || err.message}`);
        res.writeHead(500, { "Content-Type": "application/json" }).end(JSON.stringify({ error: "lyric proxy error" }));
      }
      return;
    }

    // 控制接口：/control?action=next|previous -> 返回下一首/上一首的 music metadata（方便设备直接切歌）
    if (pathname === "/control") {
      const action = (parsed.query.action || "").toString();
      if (!action || !navidromeClient) {
        res.writeHead(400, { "Content-Type": "application/json" }).end(JSON.stringify({ error: "missing action or navidrome client not ready" }));
        return;
      }
      try {
        let song = null;
        if (action === "next") song = nowPlaying.next();
        else if (action === "previous" || action === "prev") song = nowPlaying.previous();
        else {
          res.writeHead(400, { "Content-Type": "application/json" }).end(JSON.stringify({ error: "invalid action" }));
          return;
        }
        if (!song) {
          res.writeHead(404, { "Content-Type": "application/json" }).end(JSON.stringify({ error: "no more songs in this direction" }));
          return;
        }
        const audio_url = `http://${PROXY_SERVER_IP}:${HTTP_BRIDGE_PORT}/stream_proxy?songId=${encodeURIComponent(song.id)}&force_cbr=1`;
        const lyricCandidates = await navidromeClient.getLyrics({ id: song.id }).catch(()=>[]);
        const lyric_url = (Array.isArray(lyricCandidates) && lyricCandidates.length > 0) ? `http://${PROXY_SERVER_IP}:${HTTP_BRIDGE_PORT}/lyric_proxy?songId=${encodeURIComponent(song.id)}` : "";
        const payload = { id: song.id, title: song.title, artist: song.artist, album: song.album, audio_url, lyric_url, current_index: nowPlaying.currentIndex };
        res.writeHead(200, { "Content-Type": "application/json" }).end(JSON.stringify(payload));
        return;
      } catch (err) {
        logger.error(`[Control] failed: ${err && err.message}`);
        res.writeHead(500, { "Content-Type": "application/json" }).end(JSON.stringify({ error: "control failed" }));
      }
      return;
    }

    // 读取当前播放队列状态：/now_playing
    if (pathname === "/now_playing") {
      const q = nowPlaying.toArray().map((s, idx) => ({ idx, id: s.id, title: s.title, artist: s.artist, album: s.album }));
      const cur = nowPlaying.getCurrentSong();
      res.writeHead(200, { "Content-Type": "application/json" }).end(JSON.stringify({ current: cur, queue: q, current_index: nowPlaying.currentIndex }));
      return;
    }

    // HTTP 播放入口：/http_play?artist=...&album=...&song=...&playlist=...&random=1
    // 与 MCP play 功能等价，返回类似 /stream_pcm 的 payload
    if (pathname === "/http_play") {
      const params = parsed.query || {};
      try {
        const artist = params.artist;
        const album = params.album;
        const playlist = params.playlist;
        const song = params.song;
        const random = params.random === "1" || params.random === "true" || params.random === "yes";
        const result = await handlePlay({ artist, album, playlist, song, random });
        // handlePlay 返回 MCP 内容 结构，这里我们将其转换为 HTTP JSON：若返回 playlist 则输出 playlist+audio_url（首曲）
        if (!result || result.isError) {
          res.writeHead(500, { "Content-Type": "application/json" }).end(JSON.stringify({ error: "play failed", detail: result && result.content || null }));
          return;
        }
        // result.content 可能包含 playlist 和 music，直接返回第一个 playlist/music 元素组合
        // 约定：如果包含 playlist, 以 playlist 为主，并带 audio_url 指向当前曲目
        const contents = result.content || [];
        // Find playlist content
        const playlistContent = contents.find(c => c && c.type === "playlist");
        const musicContent = contents.find(c => c && c.type === "music");
        if (playlistContent) {
          // convert tracks to playlist array with audio_url (proxy)
          const tracks = playlistContent.tracks.map((t, idx) => {
            const id = t.url && t.url.includes('songId=') ? (new URL(t.url)).searchParams.get('songId') : null;
            return {
              id,
              title: t.title,
              artist: t.artist,
              album: t.album,
              audio_url: t.url,
              lyric_url: t.lyric_url || (id ? `http://${PROXY_SERVER_IP}:${HTTP_BRIDGE_PORT}/lyric_proxy?songId=${encodeURIComponent(id)}` : "")
            };
          });
          // current index from nowPlaying
          const cur = nowPlaying.getCurrentSong();
          const lyricCandidates = cur ? await navidromeClient.getLyrics({ id: cur.id }).catch(()=>[]) : [];
          const lyric_url = (Array.isArray(lyricCandidates) && lyricCandidates.length > 0) ? `http://${PROXY_SERVER_IP}:${HTTP_BRIDGE_PORT}/lyric_proxy?songId=${encodeURIComponent(cur.id)}` : "";
          const payload = { artist: cur ? cur.artist : (artist||""), title: `播放队列 (${tracks.length})`, audio_url: tracks[nowPlaying.currentIndex >=0 ? nowPlaying.currentIndex : 0].audio_url, lyric_url, playlist: tracks, playlist_type: "http_play", random: random, current_index: nowPlaying.currentIndex };
          res.writeHead(200, { "Content-Type": "application/json" }).end(JSON.stringify(payload));
          return;
        } else if (musicContent) {
          res.writeHead(200, { "Content-Type": "application/json" }).end(JSON.stringify(musicContent));
          return;
        } else {
          // fallback: return raw result
          res.writeHead(200, { "Content-Type": "application/json" }).end(JSON.stringify(result));
          return;
        }
      } catch (err) {
        logger.error(`[HTTP_PLAY] failed: ${err && err.message}`);
        res.writeHead(500, { "Content-Type": "application/json" }).end(JSON.stringify({ error: "http_play failed" }));
      }
      return;
    }

    // 其他：404
    res.writeHead(404, { "Content-Type": "application/json" }).end(JSON.stringify({ error: "not found" }));
  });
  server.on("error", (err) => { logger.error(`[HTTP Bridge] server error: ${err.stack || err.message}`); });
  server.listen(HTTP_BRIDGE_PORT, "0.0.0.0", () => { logger.info(`[HTTP Bridge] listening on http://0.0.0.0:${HTTP_BRIDGE_PORT}`); });
}

// ---------- MCP 工具与响应转换 ----------
const mcpServer = new McpServer({ name: "NavidromeMusicPlayer", version: "4.2.0" });

function proxyStreamUrlForSongId(songId) {
    return `http://${PROXY_SERVER_IP}:${HTTP_BRIDGE_PORT}/stream_proxy?songId=${encodeURIComponent(songId)}&force_cbr=1`;
}
function songToMusicContent(song) {
    if (!song) return null;
    return {
        type: "music",
        url: proxyStreamUrlForSongId(song.id),
        title: song.title,
        artist: song.artist,
        album: song.album,
        cover_art_url: song.coverArt ? proxyStreamUrlForSongId(song.coverArt) : undefined,
        sample_rate: 44100,
        channels: 2
    };
}
function songsToPlaylistContent(songs) {
    if (!songs || songs.length === 0) return null;
    const tracks = songs.slice(0, 100).map(s => ({
        url: proxyStreamUrlForSongId(s.id),
        title: s.title,
        artist: s.artist,
        album: s.album,
        cover_art_url: s.coverArt ? proxyStreamUrlForSongId(s.coverArt) : undefined,
        lyric_url: `http://${PROXY_SERVER_IP}:${HTTP_BRIDGE_PORT}/lyric_proxy?songId=${encodeURIComponent(s.id)}`
    }));
    return { type: "playlist", tracks };
}

// Play 工具（MCP）核心逻辑
async function handlePlay({ artist, album, playlist, song, random }) {
  const notReady = checkInitialized(); if (notReady) return notReady;
  try {
      let songsToQueue = [];

      if (playlist) {
          logger.info(`[Tool] Play: Handling playlist request for "${playlist}"`);
          const lists = await navidromeClient.getPlaylists();
          const target = lists.find(p => p.name && p.name.toLowerCase() === playlist.trim().toLowerCase());
          if (!target) throw new Error(`找不到播放列表: ${playlist}`);
          songsToQueue = await navidromeClient.getPlaylistSongs({ playlistId: target.id });
      }
      else if (album) {
          logger.info(`[Tool] Play: Handling album request for "${album}" by artist "${artist || 'any'}"`);
          const query = artist ? `${artist} ${album}` : album;
          const searchResults = await navidromeClient.search3({ query, albumCount: 1, songCount: 0, artistCount: 0 });
          if (!searchResults.albums || searchResults.albums.length === 0) throw new Error(`找不到专辑: ${album}`);
          const targetAlbum = searchResults.albums[0];
          songsToQueue = await navidromeClient.getAlbum({ id: targetAlbum.id });
      }
      else if (artist) {
          logger.info(`[Tool] Play: Handling artist request for "${artist}"`);
          const searchResults = await navidromeClient.search3({ query: artist, artistCount: 5, songCount: 0, albumCount: 0 });
          if (!searchResults.artists || searchResults.artists.length === 0) {
            const sr = await navidromeClient.search3({ query: artist, count: 100 });
            let songs = sr.songs || [];
            songs = songs.filter(s => s.artist && normalizeText(s.artist).includes(normalizeText(artist)));
            if (songs.length === 0) throw new Error(`找不到艺术家: ${artist}`);
            songsToQueue = songs;
          } else {
            const artistId = searchResults.artists[0].id;
            songsToQueue = await navidromeClient.getArtist({ id: artistId });
            try {
              const sr2 = await navidromeClient.search3({ query: artist, count: 500 });
              if (sr2 && Array.isArray(sr2.songs)) {
                const map = new Map();
                for (const s of songsToQueue) if (s && s.id) map.set(s.id, s);
                for (const s of sr2.songs) if (s && s.id && !map.has(s.id)) map.set(s.id, s);
                songsToQueue = Array.from(map.values());
              }
            } catch(e) { /* ignore */ }
          }
      }
      else if (song) {
          logger.info(`[Tool] Play: Handling song request for "${song}"`);
          const searchResults = await navidromeClient.search3({ query: song, count: 20 });
          if (!searchResults.songs || searchResults.songs.length === 0) throw new Error(`找不到歌曲: ${song}`);
          let best = searchResults.songs[0];
          const ql = song.trim().toLowerCase();
          for (const s of searchResults.songs) {
              if (s.title && s.title.toLowerCase() === ql) { best = s; break; }
          }
          songsToQueue = [best];
      }
      else {
          logger.info(`[Tool] Play: Handling random request`);
          songsToQueue = await navidromeClient.getRandomSongs({ size: 50 });
          random = true;
      }

      if (!songsToQueue || songsToQueue.length === 0) throw new Error("找不到任何可播放的歌曲。");

      nowPlaying.setQueue(songsToQueue, false);
      if (random) nowPlaying.shuffle();

      const firstSong = nowPlaying.next();
      if (!firstSong) throw new Error("队列为空，无法开始播放。");

      const playlistContent = songsToPlaylistContent(nowPlaying.toArray());
      const musicContent = songToMusicContent(firstSong);

      const content = [];
      if (playlistContent) content.push(playlistContent);
      if (musicContent) content.push(musicContent);

      return { content };
  } catch (err) {
      logger.error("Play tool failed: " + (err.stack || err.message));
      return { isError: true, content: [{ type: "text", text: `播放失败: ${err.message}` }] };
  }
}

// 注册 MCP 工具（含兼容名称）
mcpServer.tool("play", "播放音乐、歌单或专辑。", {
  type: "object", properties: { artist: { type: "string" }, album: { type: "string" }, playlist: { type: "string" }, song: { type: "string" }, random: { type: "boolean" } }
}, async (params) => handlePlay(params));
mcpServer.tool("navidrome_xzcli_play", "兼容: 播放音乐", {
  type: "object", properties: { artist: { type: "string" }, album: { type: "string" }, playlist: { type: "string" }, song: { type: "string" }, random: { type: "boolean" } }
}, async (params) => handlePlay(params));

// playback_control
async function handlePlaybackControl({ action }) {
  const notReady = checkInitialized(); if (notReady) return notReady;
  try {
      let song = (action === 'next') ? nowPlaying.next() : nowPlaying.previous();
      if (!song) return { content: [{ type: "text", text: `已经是${action === 'next' ? '最后' : '第一'}首歌了。` }] };
      return { content: [songToMusicContent(song)] };
  } catch (err) {
      logger.error("playback_control tool failed: " + (err && err.message));
      return { isError: true, content: [{ type: "text", text: `控制失败: ${err.message}` }] };
  }
}
mcpServer.tool("playback_control", "控制当前播放。'action'可以是'next'或'previous'", {
  type: "object", properties: { action: { type: "string", enum: ["next","previous"] } }, required: ["action"]
}, async (p) => handlePlaybackControl(p));
mcpServer.tool("navidrome_xzcli_playback_control", "兼容: 控制播放", {
  type: "object", properties: { action: { type: "string", enum: ["next","previous"] } }, required: ["action"]
}, async (p) => handlePlaybackControl(p));

mcpServer.tool("get_playlists", "查询播放列表", {}, async () => {
  const notReady = checkInitialized(); if (notReady) return notReady;
  try {
    const lists = await navidromeClient.getPlaylists();
    const results = lists.map(p => p.name);
    return { content: [{ type: "text", text: `我找到了这些播放列表：${results.join('，') || '没有找到任何播放列表'}。你想播放哪一个？` }] };
  } catch (err) {
    logger.error("get_playlists failed: " + (err.stack || err.message));
    return { isError: true, content: [{ type: "text", text: "获取播放列表失败。" }] };
  }
});

// now_playing 工具
mcpServer.tool("now_playing", "获取当前播放队列", {}, async () => {
  const notReady = checkInitialized(); if (notReady) return notReady;
  const cur = nowPlaying.getCurrentSong();
  const q = nowPlaying.toArray().map((s, idx) => ({ idx, id: s.id, title: s.title, artist: s.artist }));
  const text = `当前曲目：${cur ? `${cur.title} - ${cur.artist}` : '无'}，队列长度：${q.length}`;
  return { content: [{ type: "text", text }, { type: "json", data: { current: cur, queue: q } }] };
});

// ---------------- main --------------------
async function main() {
  process.on('unhandledRejection', (reason, promise) => {
    logger.error('----- UNHANDLED REJECTION -----');
    logger.error('Unhandled Rejection at:', promise, 'reason:', reason instanceof Error ? reason.stack : reason);
    logger.error('-------------------------------');
  });
  
  process.on('uncaughtException', (err, origin) => {
    if (err.code === 'EPIPE') {
      logger.warn(`Caught a global EPIPE error, which is safe to ignore. Origin: ${origin}`);
      return; 
    }
    
    logger.error('----- UNCAUGHT EXCEPTION -----');
    logger.error(`Caught exception: ${err}\n` + `Exception origin: ${origin}\n` + `Stack: ${err.stack}`);
    logger.error('------------------------------');
    logger.error('Fatal error detected. Exiting process.');
    
    process.exit(1);
  });

  logger.info("Starting Navidrome MCP service...");
  const ok = await initializeNavidrome();
  if (!ok) {
    logger.error("Navidrome initialization failed; tools will report service not ready.");
  }

  const transport = new StdioServerTransport();
  await mcpServer.connect(transport);
  logger.info("MCP server connected via stdio.");

  startHttpBridge();
}

if (import.meta.url && fileURLToPath(import.meta.url) === process.argv[1]) {
  main().catch(err => {
    logger.error("Fatal error in main: " + (err && (err.stack || err.message)));
    process.exit(1);
  });
}
