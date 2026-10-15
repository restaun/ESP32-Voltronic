"use strict";
const byId = id => document.getElementById(id);
const result = byId("result");
for (const [buttonId, fieldId, description] of [
    ["show-password", "password", "de red"],
    ["show-web-password", "web-password", "web"]
]) {
    byId(buttonId).addEventListener("click", () => {
        const show = byId(fieldId).type !== "text";
        byId(fieldId).type = show ? "text" : "password";
        byId(buttonId).setAttribute("aria-pressed", String(show));
        byId(buttonId).textContent = `${show ? "Ocultar" : "Mostrar"} contraseña ${description}`;
    });
}
function message(text, error = false) {
    result.textContent = text;
    result.dataset.error = String(error);
}
function updateMode() {
    const ap = byId("mode").value === "ap";
    byId("scan-area").hidden = ap;
    byId("password").minLength = 8;
    byId("password").maxLength = ap ? 63 : 64;
    byId("password").required = ap;
    byId("password-help").textContent = ap
        ? "Clave WPA2 de 8 a 63 caracteres. La red admite hasta cuatro clientes."
        : "Déjala vacía solo si la red es abierta.";
}
byId("mode").addEventListener("change", updateMode);
byId("networks").addEventListener("change", () => {
    if (byId("networks").value) byId("ssid").value = byId("networks").value;
});
async function request(url, options = {}) {
    const response = await fetch(url, {cache:"no-store", signal:AbortSignal.timeout(25000), ...options});
    if (!response.ok) throw new Error(await response.text());
    return response;
}
byId("scan").addEventListener("click", async () => {
    byId("scan").disabled = true;
    message("Buscando redes...");
    try {
        const networks = await (await request("/api/wifi/scan")).json();
        const placeholder = document.createElement("option");
        placeholder.value = "";
        placeholder.textContent = "Elige una red o escribe su nombre";
        byId("networks").replaceChildren(placeholder);
        for (const network of networks) {
            const option = document.createElement("option");
            option.value = network.ssid;
            option.textContent = `${network.ssid} (${network.rssi} dBm, ${network.secured ? "con clave" : "abierta"})`;
            byId("networks").append(option);
        }
        message(networks.length ? "Selecciona una red de la lista." : "No se encontraron redes. Puedes escribir un SSID oculto.");
    } catch (error) {
        message("No se pudieron buscar redes: " + error.message, true);
    } finally {
        byId("scan").disabled = false;
    }
});
async function save(url, data) {
    byId("save").disabled = true;
    byId("reset").disabled = true;
    byId("scan").disabled = true;
    message("Guardando configuración...");
    try {
        const response = await request(url, {method:"POST", headers:{"Content-Type":"application/json"}, body:JSON.stringify(data)});
        message(await response.text());
        byId("password").value = "";
        byId("web-password").value = "";
        for (const [buttonId, fieldId, description] of [
            ["show-password", "password", "de red"],
            ["show-web-password", "web-password", "web"]
        ]) {
            byId(fieldId).type = "password";
            byId(buttonId).setAttribute("aria-pressed", "false");
            byId(buttonId).textContent = `Mostrar contraseña ${description}`;
        }
    } catch (error) {
        message("No se confirmó el cambio: " + error.message + ". Comprueba la conexión antes de reintentar.", true);
        byId("save").disabled = false;
        byId("reset").disabled = false;
        byId("scan").disabled = false;
    }
}
byId("wifi-form").addEventListener("submit", async event => {
    event.preventDefault();
    if (!byId("wifi-form").reportValidity()) return;
    const password = byId("password").value;
    const length = new TextEncoder().encode(password).length;
    const ap = byId("mode").value === "ap";
    const validPassword = (length >= 8 && length <= 63) ||
        (!ap && (length === 0 || /^[a-fA-F0-9]{64}$/.test(password)));
    const webLength = new TextEncoder().encode(byId("web-password").value).length;
    const ssidLength = new TextEncoder().encode(byId("ssid").value).length;
    if (!validPassword || ssidLength < 1 || ssidLength > 32 ||
        (webLength !== 0 && (webLength < 8 || webLength > 48))) {
        message("Revisa SSID (1 a 32 bytes), clave de red (8 a 63, o vacía para red abierta) y clave web (8 a 48, o vacía para conservar).", true);
        return;
    }
    await save("/api/wifi", {
        mode:byId("mode").value, ssid:byId("ssid").value,
        password, web_password:byId("web-password").value
    });
});
byId("reset").addEventListener("click", async () => {
    if (confirm("¿Olvidar Wi-Fi y contraseña web? Tendrás que configurar de nuevo en la pantalla táctil.")) {
        await save("/api/wifi/reset", {confirm:"RESET"});
    }
});
async function loadStatus() {
    try {
        const status = await (await request("/api/status")).json();
        byId("network-status").textContent = status.network_mode === "ap"
            ? `Red propia activa · ${status.wifi} · Configura la red o abre el panel.`
            : `Conectado a Wi-Fi · ${status.wifi}`;
    } catch (error) {
        byId("network-status").textContent = "No se pudo consultar la red: " + error.message;
    }
}
updateMode();
loadStatus();
