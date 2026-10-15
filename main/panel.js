"use strict";
(() => {
    const $ = id => document.getElementById(id);
    const finite = value => typeof value === "number" && Number.isFinite(value);
    const format = (value, unit, digits = 1) => finite(value)
        ? value.toLocaleString("es-ES", {maximumFractionDigits: digits}) + " " + unit : "Sin datos";
    const modes = {P:"Encendido", S:"En espera", L:"En red", B:"En batería", F:"Fallo",
        H:"Ahorro de energía", D:"Apagado", C:"Cargando", Y:"Bypass", E:"Modo ECO"};
    let panel = null, host = null, pending = false, selected = null, uploading = false;
    const cards = [...document.querySelectorAll("[data-card]")];
    const note = "Solo datos del Axpert. Potencia de entrada AC no disponible. Corriente neta y potencia de batería calculadas: carga menos descarga; positivo = carga. No es una medición directa del BMS.";
    const labels = {
        ac_input_voltage_v:["Tensión de entrada","V"], ac_input_frequency_hz:["Frecuencia de entrada","Hz"],
        ac_output_voltage_v:["Tensión de salida","V"], ac_output_frequency_hz:["Frecuencia de salida","Hz"],
        ac_output_apparent_power_va:["Potencia aparente","VA"], ac_output_power_w:["Potencia activa","W"],
        ac_output_load_percent:["Carga de salida","%"], dc_bus_voltage_v:["Bus DC","V"],
        battery_voltage_v:["Tensión de batería","V"], battery_charge_current_a:["Corriente de carga","A"],
        battery_discharge_current_a:["Corriente de descarga","A"], battery_soc_percent:["SOC informado por Axpert","%"],
        battery_net_current_a:["Corriente neta calculada (+ carga)","A"], battery_power_estimate_w:["Potencia calculada (+ carga)","W"],
        bms_soc_percent:["SOC BMS comunicado al Axpert","%"], inverter_temperature_c:["Temperatura del inversor","°C"],
        pv_input_current_a:["Corriente FV informada","A"], pv_input_voltage_v:["Tensión FV","V"],
        pv_charging_power_w:["Potencia de carga FV","W"], solar_charger_battery_voltage_v:["Tensión batería SCC","V"],
        pv_total_energy_wh:["Energía FV acumulada","Wh"], load_total_energy_wh:["Energía salida acumulada","Wh"]
    };
    const keys = {
        solar:["pv_charging_power_w","pv_input_voltage_v","pv_input_current_a","solar_charger_battery_voltage_v","pv_total_energy_wh"],
        grid:["ac_input_voltage_v","ac_input_frequency_hz"],
        house:["ac_output_power_w","ac_output_apparent_power_va","ac_output_voltage_v","ac_output_frequency_hz","ac_output_load_percent","load_total_energy_wh"],
        battery:["battery_soc_percent","battery_voltage_v","battery_charge_current_a","battery_discharge_current_a","battery_net_current_a","battery_power_estimate_w","bms_soc_percent"],
        inverter:["inverter_temperature_c","dc_bus_voltage_v","ac_output_power_w"]
    };
    function layout() {
        const bounds = document.querySelector("main").getBoundingClientRect();
        const rect = name => cards.find(c => c.dataset.card === name).getBoundingClientRect();
        for (const [id,from,to] of [["solar-flow","solar","inverter"],["grid-flow","grid","inverter"],
            ["house-flow","inverter","house"],["battery-flow","battery","inverter"]]) {
            const a = rect(from), b = rect(to);
            let ax = a.x + a.width / 2 - bounds.x, ay = a.y + a.height / 2 - bounds.y;
            let bx = b.x + b.width / 2 - bounds.x, by = b.y + b.height / 2 - bounds.y;
            if (to === "inverter" && (from === "solar" || from === "grid")) {
                const d = bx > ax ? 1 : -1;
                ax += d * a.width / 2;
                bx -= d * b.width / 2;
                by = b.y - bounds.y + b.height * (from === "solar" ? 0.3 : 0.75);
                const bend = ax + (bx - ax) * (from === "solar" ? 0.35 : 0.7);
                $(id).setAttribute("d", `M${ax} ${ay}H${bend}V${by}H${bx}`);
                continue;
            }
            if (Math.abs(ax - bx) > Math.abs(ay - by)) {
                const d = bx > ax ? 1 : -1; ax += d * a.width / 2; bx -= d * b.width / 2;
                $(id).setAttribute("d", `M${ax} ${ay}H${(ax+bx)/2}V${by}H${bx}`);
            } else {
                const d = by > ay ? 1 : -1; ay += d * a.height / 2; by -= d * b.height / 2;
                $(id).setAttribute("d", `M${ax} ${ay}V${(ay+by)/2}H${bx}V${by}`);
            }
        }
    }
    new ResizeObserver(layout).observe(document.querySelector("main"));
    function detail() {
        if (!selected) return;
        $("detail-title").textContent = cards.find(c => c.dataset.card === selected).querySelector("span:not(.icon)")?.textContent || "ESP32";
        $("detail-values").replaceChildren();
        const row = (name, value) => {
            const dt = document.createElement("dt"), dd = document.createElement("dd");
            dt.textContent = name; dd.textContent = value; $("detail-values").append(dt, dd);
        };
        if (selected === "esp32") {
            row("Wi-Fi / IP", host?.wifi || "Sin conexión");
            row("Nombre", host?.hostname || "Sin datos");
            row("Firmware", host?.version || "Sin datos");
            row("USB HID", host ? (host.hid ? "Detectado" : "No detectado") : "Sin conexión");
            row("Identificación PI30", panel?.connected ? "Validada" : "Sin confirmar");
        } else {
            for (const key of keys[selected]) row(labels[key][0], format(panel?.values[key], labels[key][1]));
            if (selected === "inverter") {
                row("Modo", modes[panel?.mode] || "Sin datos");
                row("Bits de estado QPIWS", panel?.warning_bits || "Sin datos");
            }
            if (selected === "battery") row("Enlace BMS informado por Axpert",
                panel?.bms_available ? (panel.bms_connected ? "Conectado" : "Desconectado") : "Sin datos");
        }
        $("detail-note").textContent = note;
    }
    function render() {
        const v = panel?.values || {};
        $("solar-power").textContent = format(v.pv_charging_power_w, "W", 0);
        $("solar-detail").textContent = format(v.pv_input_voltage_v,"V") + " · " + format(v.pv_input_current_a,"A");
        $("grid-voltage").textContent = format(v.ac_input_voltage_v,"V");
        $("grid-detail").textContent = format(v.ac_input_frequency_hz,"Hz") + "\nPotencia AC de entrada: no disponible";
        $("output-power").textContent = format(v.ac_output_power_w,"W",0);
        $("output-detail").textContent = format(v.ac_output_voltage_v,"V") + " · " + format(v.ac_output_apparent_power_va,"VA",0) + "\nCarga: " + format(v.ac_output_load_percent,"%",0);
        $("battery-soc").textContent = format(v.battery_soc_percent,"%",0);
        const net = v.battery_net_current_a;
        $("battery-detail").textContent = format(v.battery_voltage_v,"V") + " · " + format(net,"A") + "\n" +
            (finite(net) ? (net > 0 ? "Cargando" : net < 0 ? "Descargando" : "En reposo") + " · " + format(finite(v.battery_power_estimate_w) ? Math.abs(v.battery_power_estimate_w) : null,"W",0) + " (calc.)" : "Sin datos");
        $("battery-source").textContent = "SOC Axpert" + (finite(v.bms_soc_percent) ? " · BMS vía Axpert: " + format(v.bms_soc_percent,"%",0) : "");
        $("model").textContent = panel?.model || "Axpert VMIII";
        $("mode").textContent = modes[panel?.mode] || "Sin datos";
        $("temperature").textContent = format(v.inverter_temperature_c,"°C");
        $("inverter-output").textContent = panel?.connected ? "USB HID · PI30" : "USB sin confirmar";
        $("usb").textContent = !host ? "Sin conexión" : panel?.connected ? "USB conectado" : host.hid ? "HID detectado" : "Sin USB";
        $("host-detail").textContent = host ? (host.wifi + "\n" + host.hostname + " · v" + host.version) : "No se puede acceder al ESP32";
        const age = panel?.measurements_age_ms;
        $("status").textContent = !host ? "Sin conexión con el ESP32" :
            finite(age) && age >= 0 ? "Datos del Axpert por USB · hace " + (age/1000).toFixed(1) + " s · solo lectura" : "Sin medidas recientes del Axpert";
        $("status").classList.toggle("error", !host || !finite(age) || age < 0);
        const clock = panel?.inverter_clock;
        $("clock").textContent = clock && /^\d{14}$/.test(clock)
            ? `${clock.slice(8,10)}:${clock.slice(10,12)}\n${clock.slice(6,8)}/${clock.slice(4,6)}/${clock.slice(0,4)}` : "Sin reloj Axpert";
        $("energy").textContent = "Acumulado FV: " + format(finite(v.pv_total_energy_wh) ? v.pv_total_energy_wh/1000 : null,"kWh") +
            " · Salida: " + format(finite(v.load_total_energy_wh) ? v.load_total_energy_wh/1000 : null,"kWh");
        $("notice").textContent = panel?.warning_bits ? "Estado QPIWS: " + panel.warning_bits + " · Pulsa Axpert para detalles" : "Sin estado QPIWS reciente";
        for (const card of cards) {
            const key = {solar:"pv_charging_power_w",grid:"ac_input_voltage_v",house:"ac_output_power_w",battery:"battery_soc_percent",inverter:"inverter_temperature_c"}[card.dataset.card];
            card.classList.toggle("unavailable", key ? !finite(v[key]) : !host);
        }
        $("solar-flow").classList.toggle("active", finite(v.pv_charging_power_w) && v.pv_charging_power_w > 0);
        $("house-flow").classList.toggle("active", finite(v.ac_output_power_w) && v.ac_output_power_w > 0);
        $("battery-flow").classList.toggle("active", finite(net) && net !== 0);
        $("battery-flow").classList.toggle("reverse", finite(net) && net > 0);
        // Input voltage alone does not prove energy is flowing from the grid.
        detail();
    }
    async function get(url) {
        const response = await fetch(url, {cache:"no-store", signal:AbortSignal.timeout(7000)});
        if (!response.ok) throw new Error("HTTP " + response.status + " al consultar " + url);
        return response.json();
    }
    async function refresh() {
        if (pending || uploading) return;
        pending = true;
        try {
            const result = await Promise.all([get("/api/panel"), get("/api/status")]);
            [panel, host] = result;
        } catch (error) {
            panel = null; host = null;
            $("ota-status").textContent = error.message;
        } finally {
            pending = false; render();
            setTimeout(refresh, 3000);
        }
    }
    for (const card of cards) card.addEventListener("click", () => { selected = card.dataset.card; detail(); $("detail").showModal(); });
    $("close-detail").onclick = () => { selected = null; $("detail").close(); };
    $("maintenance").onclick = () => $("tools").showModal();
    $("close-tools").onclick = () => $("tools").close();
    $("fullscreen").onclick = async () => {
        try { if (document.fullscreenElement) await document.exitFullscreen(); else await document.documentElement.requestFullscreen(); }
        catch (error) { $("notice").textContent = "Pantalla completa no disponible: " + error.message; }
    };
    $("refresh-raw").onclick = async () => {
        try { $("raw").textContent = JSON.stringify(await get("/api/axpert"),null,2); }
        catch (error) { $("raw").textContent = error.message; }
    };
    $("upload").onclick = async () => {
        const file = $("firmware").files[0];
        if (!file || !file.name.toLowerCase().endsWith(".bin") || file.size <= 0 || file.size > 4194304) {
            $("ota-status").textContent = "Selecciona un firmware.bin de hasta 4 MB."; return;
        }
        if (!confirm("¿Actualizar firmware y reiniciar la pantalla?")) return;
        uploading = true; $("upload").disabled = true;
        $("ota-status").textContent = "Subiendo firmware. No desconectes la alimentación.";
        try {
            const response = await fetch("/update", {method:"POST", headers:{"Content-Type":"application/octet-stream"},body:file});
            const message = await response.text();
            if (!response.ok) throw new Error(message);
            $("ota-status").textContent = message;
            setTimeout(() => location.reload(), 12000);
        } catch (error) {
            $("ota-status").textContent = "Error OTA: " + error.message;
            uploading = false; $("upload").disabled = false; setTimeout(refresh,3000);
        }
    };
    refresh();
})();
