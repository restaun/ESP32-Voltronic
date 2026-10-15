const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const source = fs.readFileSync(path.join(__dirname, "..", "main", "panel.js"), "utf8");
const html = fs.readFileSync(path.join(__dirname, "..", "main", "panel.html"), "utf8");

async function fixture(overrides = {}, failed = false) {
    const elements = new Map();
    function element() {
        return {textContent:"", children:[], disabled:false, files:[], attributes:{},
            setAttribute(name,value) { this.attributes[name] = value; },
            classList:{values:new Map(), toggle(name, value) { this.values.set(name, value); }},
            replaceChildren() { this.children = []; }, append(...items) { this.children.push(...items); },
            addEventListener(name, callback) { this[name] = callback; },
            showModal() {}, close() {}};
    }
    for (const match of html.matchAll(/\bid="([^"]+)"/g)) elements.set(match[1], element());
    const geometry = {
        solar:{x:20,y:90,width:300,height:160},grid:{x:20,y:290,width:300,height:160},
        inverter:{x:390,y:90,width:240,height:360},esp32:{x:710,y:90,width:290,height:160},
        house:{x:710,y:290,width:290,height:160},battery:{x:350,y:490,width:320,height:160}
    };
    const cards = ["solar","grid","inverter","esp32","house","battery"].map(name => ({
        ...element(), dataset:{card:name}, querySelector:() => ({textContent:name}),
        getBoundingClientRect:() => geometry[name]
    }));
    const values = {pv_charging_power_w:0, pv_input_voltage_v:0, pv_input_current_a:0,
        ac_input_voltage_v:232.5, ac_input_frequency_hz:50, ac_output_power_w:218,
        ac_output_voltage_v:229.7, ac_output_apparent_power_va:230, ac_output_load_percent:7,
        battery_voltage_v:26.1, battery_soc_percent:38, battery_net_current_a:-9,
        battery_power_estimate_w:-234.9, bms_soc_percent:38, inverter_temperature_c:40,
        pv_total_energy_wh:2033500, load_total_energy_wh:9421700, ...overrides};
    const panel = {values, model:"VMIII-3000", mode:"B", connected:true, measurements_age_ms:1200,
        inverter_clock:"20261015124533", warning_bits:"10000000000000000000000000000000",
        bms_available:true, bms_connected:true};
    const host = {wifi:"192.168.1.145", hostname:"esp32axpertvmii.local", version:"0.3.0", hid:true};
    const context = {
        document:{getElementById(id) {
            assert.ok(elements.has(id), "Unknown HTML id: " + id); return elements.get(id);
        }, querySelectorAll:() => cards, querySelector:() => ({getBoundingClientRect:() => ({x:0,y:0})}), createElement:element},
        ResizeObserver:class { constructor(callback) { this.callback = callback; } observe() { this.callback(); } },
        AbortSignal, setTimeout() {},
        fetch:async url => {
            if (failed) throw new Error("Network unavailable");
            return {ok:true,json:async () => url === "/api/panel" ? panel : host};
        }
    };
    vm.runInNewContext(source, context);
    await new Promise(resolve => setImmediate(resolve));
    return {elements, cards};
}

test("real zero solar is shown as zero, not missing", async () => {
    const {elements} = await fixture();
    assert.equal(elements.get("solar-power").textContent, "0 W");
    assert.equal(elements.get("solar-flow").classList.values.get("active"), false);
});
test("positive output and battery discharge animate only known flows", async () => {
    const {elements} = await fixture();
    assert.equal(elements.get("output-power").textContent, "218 W");
    assert.equal(elements.get("inverter-output").textContent, "USB HID · PI30");
    assert.equal(elements.get("house-flow").classList.values.get("active"), true);
    assert.equal(elements.get("battery-flow").classList.values.get("active"), true);
    assert.equal(elements.get("battery-flow").classList.values.get("reverse"), false);
    assert.equal(elements.get("grid-flow").classList.values.get("active"), undefined);
    assert.match(elements.get("battery-detail").textContent, /Descargando.*235 W \(calc\.\)/);
});
test("charge flow reverses and is explicitly calculated", async () => {
    const {elements} = await fixture({battery_net_current_a:5, battery_power_estimate_w:130.5});
    assert.equal(elements.get("battery-flow").classList.values.get("reverse"), true);
    assert.match(elements.get("battery-detail").textContent, /Cargando.*calc/);
});
test("null and absent measurements never look like zero", async () => {
    const {elements,cards} = await fixture({pv_charging_power_w:null, ac_output_power_w:undefined,
        battery_soc_percent:null, battery_net_current_a:null, battery_power_estimate_w:null});
    assert.equal(elements.get("solar-power").textContent, "Sin datos");
    assert.equal(elements.get("output-power").textContent, "Sin datos");
    assert.equal(elements.get("battery-soc").textContent, "Sin datos");
    assert.equal(elements.get("battery-flow").classList.values.get("active"), false);
    assert.equal(cards[0].classList.values.get("unavailable"), true);
});
test("fetch errors clear measurements and identify disconnection", async () => {
    const {elements} = await fixture({}, true);
    assert.equal(elements.get("output-power").textContent, "Sin datos");
    assert.equal(elements.get("usb").textContent, "Sin conexión");
    assert.match(elements.get("status").textContent, /Sin conexión/);
    assert.equal(elements.get("house-flow").classList.values.get("active"), false);
    assert.match(elements.get("ota-status").textContent, /Network unavailable/);
});
test("missing battery voltage does not turn calculated power into zero", async () => {
    const {elements} = await fixture({battery_voltage_v:null, battery_power_estimate_w:null});
    assert.match(elements.get("battery-detail").textContent, /Descargando · Sin datos \(calc\.\)/);
    assert.doesNotMatch(elements.get("battery-detail").textContent, /0 W/);
});
test("clock uses inverter date and missing input power is explicit", async () => {
    const {elements} = await fixture();
    assert.equal(elements.get("clock").textContent, "12:45\n15/10/2026");
    assert.match(elements.get("grid-detail").textContent, /no disponible/);
    assert.match(elements.get("battery-source").textContent, /SOC Axpert.*BMS vía Axpert/);
});
test("card details expose metrics without direct BMS claims", async () => {
    const {elements,cards} = await fixture();
    cards.find(c => c.dataset.card === "battery").click();
    const detail = elements.get("detail-values").children.map(e => e.textContent).join(" ");
    assert.match(detail, /SOC informado por Axpert/);
    assert.match(detail, /calculada/);
    assert.match(detail, /BMS comunicado al Axpert/);
    assert.match(elements.get("detail-note").textContent, /No es una medición directa del BMS/);
});
test("solar and grid use distinct inverter ports and independent bends", async () => {
    const {elements} = await fixture();
    const solar = elements.get("solar-flow").attributes.d;
    const grid = elements.get("grid-flow").attributes.d;
    assert.equal(solar, "M320 170H344.5V198H390");
    assert.equal(grid, "M320 370H369V360H390");
    assert.notEqual(solar.match(/H([^V]+)/)[1], grid.match(/H([^V]+)/)[1]);
});
