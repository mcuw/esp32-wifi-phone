import { component$, useStore, useVisibleTask$, $ } from "@qwik.dev/core";
import { api } from "../../lib/api";

type Net = { s: string; r: number; a: number };
type Link = "connecting" | "ready" | "failed" | "saved";

const bars = (rssi: number) => (rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1);

export default component$(() => {
  const st = useStore({
    link: "connecting" as Link,
    deviceId: "",
    fw: "",
    nets: [] as Net[],
    scanning: false,
    ssid: "",
    pass: "",
    name: "",
    showPass: false,
    busy: false,
    err: "",
  });

  const scan = $(async () => {
    st.scanning = true;
    try {
      const r = await api.get("/api/scan");
      st.nets = r.ok ? (JSON.parse(r.payload) as Net[]) : [];
    } catch {
      st.err = "Die Netzwerksuche hat nicht geklappt. Tippe auf „Erneut suchen“.";
    }
    st.scanning = false;
  });

  const connect = $(async () => {
    st.link = "connecting";
    st.err = "";
    try {
      const r = await api.get("/api/info");
      if (!r.ok) throw new Error(r.code);
      const info = JSON.parse(r.payload) as { id: string; name: string; fw: string };
      st.deviceId = info.id;
      st.fw = info.fw;
      st.name = info.name;
      st.link = "ready";
      await scan();
    } catch {
      st.link = "failed";
    }
  });

  useVisibleTask$(() => { connect(); }, { strategy: "document-ready" });

  const save = $(async () => {
    st.err = "";
    if (!st.ssid.trim()) { st.err = "Wähle ein WLAN aus oder gib den Namen ein."; return; }
    if (st.pass.length > 0 && st.pass.length < 8) { st.err = "Das WLAN-Passwort braucht mindestens 8 Zeichen."; return; }
    if (!st.name.trim()) { st.err = "Gib dem Telefon einen Namen."; return; }
    st.busy = true;
    try {
      const r = await api.post("/api/wifi", { ssid: st.ssid, pass: st.pass, name: st.name.trim() });
      if (r.ok) st.link = "saved";
      else st.err = `Das Telefon hat die Eingabe abgelehnt (${r.payload || r.code}).`;
    } catch {
      st.err = "Das Telefon antwortet nicht. Prüfe, ob du noch mit seinem WLAN verbunden bist.";
    }
    st.busy = false;
  });

  if (st.link === "connecting") {
    return <p class="status" role="status">Verbinde mit dem Telefon …</p>;
  }

  if (st.link === "failed") {
    return (
      <section class="card">
        <h2>Keine Verbindung zum Telefon</h2>
        <p>
          Bleibe mit dem WLAN des Telefons verbunden, auch wenn dein Handy „Kein Internet“ meldet,
          und lade die Seite neu.
        </p>
        <button class="primary" onClick$={connect}>Erneut verbinden</button>
      </section>
    );
  }

  if (st.link === "saved") {
    return (
      <section class="card ok" role="status">
        <h2>Gespeichert</h2>
        <p>
          Das Telefon startet neu und verbindet sich mit „{st.ssid}“. Sobald auf seinem Display
          die Kontaktliste erscheint, kannst du diese Seite schließen.
        </p>
        <p>Erscheint wieder die WLAN-Einrichtung, stimmt das Passwort nicht.</p>
      </section>
    );
  }

  return (
    <form preventdefault:submit onSubmit$={save} class="stack">
      <p class="meta">Telefon {st.deviceId}, Firmware {st.fw}</p>

      <fieldset>
        <legend>WLAN</legend>
        <ul class="nets" role="listbox" aria-label="Gefundene WLAN-Netze">
          {st.nets.map((n) => (
            <li key={n.s}>
              <button
                type="button"
                role="option"
                aria-selected={st.ssid === n.s}
                class={{ net: true, sel: st.ssid === n.s }}
                onClick$={() => (st.ssid = n.s)}
              >
                <span class="sig" aria-hidden="true">
                  {[1, 2, 3, 4].map((i) => <i key={i} class={{ on: i <= bars(n.r) }} style={{ height: `${4 + i * 3}px` }} />)}
                </span>
                <span class="ssid">{n.s}</span>
                <span class="tag">{n.a === 0 ? "offen" : "gesichert"}</span>
              </button>
            </li>
          ))}
        </ul>
        {st.nets.length === 0 && !st.scanning && <p class="hint">Kein Netz gefunden.</p>}
        <button type="button" class="ghost" disabled={st.scanning} onClick$={scan}>
          {st.scanning ? "Suche läuft …" : "Erneut suchen"}
        </button>
        <p class="hint">Das Telefon findet nur 2,4-GHz-Netze. Für den Sparmodus (iTWT) braucht der Router Wi-Fi 6.</p>

        <label for="ssid">Netzwerkname (auch für versteckte Netze)</label>
        <input id="ssid" value={st.ssid} maxLength={32} autoCapitalize="off" autoCorrect="off"
               onInput$={(_, el) => (st.ssid = el.value)} />

        <label for="pass">WLAN-Passwort</label>
        <div class="pw">
          <input id="pass" type={st.showPass ? "text" : "password"} value={st.pass} maxLength={63}
                 autoComplete="off" onInput$={(_, el) => (st.pass = el.value)} />
          <button type="button" class="ghost" onClick$={() => (st.showPass = !st.showPass)}>
            {st.showPass ? "Verbergen" : "Anzeigen"}
          </button>
        </div>
      </fieldset>

      <fieldset>
        <legend>Dieses Telefon</legend>
        <label for="name">Name in der Kontaktliste</label>
        <input id="name" value={st.name} maxLength={24} onInput$={(_, el) => (st.name = el.value)} />
        <p class="hint">Auf dem Telefon-Display werden Umlaute nicht dargestellt. Nutze besser „Jan“ als „Jörg“.</p>
      </fieldset>

      {st.err && <p class="error" role="alert">{st.err}</p>}
      <button type="submit" class="primary" disabled={st.busy}>
        {st.busy ? "Speichere …" : "Speichern und neu starten"}
      </button>
    </form>
  );
});
