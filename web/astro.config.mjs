import { defineConfig } from "astro/config";
import qwikdev from "@qwik.dev/astro";

// Static output -> web/dist is flashed into the ESP32's "www" SPIFFS partition.
export default defineConfig({
  integrations: [qwikdev()],
  output: "static",
  build: {
    inlineStylesheets: "always",   // fewer files for the device to serve
    assets: "a",                   // short asset dir name (SPIFFS path length)
  },
});
