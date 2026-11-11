/**
 * Tiny client for the phone's setup API (plain HTTP/JSON, same origin).
 * `?device=<ip>` points the page at a real unit while running `npm run dev`.
 */
export type ApiResult = { ok: boolean; code: string; payload: string };

function base(): string {
  const d = new URLSearchParams(location.search).get("device");
  return d ? `http://${d}` : "";
}

async function call(method: "GET" | "POST", path: string, body?: unknown, timeoutMs = 15000): Promise<ApiResult> {
  const ctl = new AbortController();
  const timer = window.setTimeout(() => ctl.abort(), timeoutMs);
  try {
    const res = await fetch(base() + path, {
      method,
      signal: ctl.signal,
      // text/plain keeps cross-origin dev requests free of CORS preflights
      headers: body === undefined ? undefined : { "Content-Type": "text/plain" },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    return { ok: res.ok, code: String(res.status), payload: await res.text() };
  } finally {
    window.clearTimeout(timer);
  }
}

export const api = {
  get: (path: string) => call("GET", path),
  post: (path: string, body: unknown) => call("POST", path, body),
};
