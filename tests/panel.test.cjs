const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const source = fs.readFileSync(path.join(__dirname, "..", "main", "panel.js"), "utf8");
const html = fs.readFileSync(path.join(__dirname, "..", "main", "panel.html"), "utf8");
test("inverter uses the original drawing without the supplied photo", () => {
    assert.match(html, /class="inverter-shell"/);
    assert.match(html, /class="vents"/);
    assert.match(html, /class="lcd"/);
    assert.match(html, /id="mode"/);
    assert.match(html, /id="temperature"/);
    assert.doesNotMatch(html, /axpert-front|inverter-photo/);
});

async function fixture(overrides = {}, failed = false, options = {}) {
    const elements = new Map();
    function element() {
        return {textContent:"", children:[], disabled:false, files:[], attributes:{}, value:"", hidden:false,
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
    const requests = [], confirmations = [], timers = [];
    const configuration = options.configuration || {settings:[],additional:[],job:{id:0,state:"idle",message:""},busy:false};
    const context = {
        document:{getElementById(id) {
            assert.ok(elements.has(id), "Unknown HTML id: " + id); return elements.get(id);
        }, querySelectorAll:() => cards, querySelector:() => ({getBoundingClientRect:() => ({x:0,y:0})}), createElement:element},
        ResizeObserver:class { constructor(callback) { this.callback = callback; } observe() { this.callback(); } },
        AbortSignal, setTimeout(callback) { timers.push(callback); },
        confirm(message) { confirmations.push(message); return options.confirm !== false; },
        fetch:async (url, init = {}) => {
            requests.push({url, init});
            if (failed) throw new Error("Network unavailable");
            if (url === "/api/settings") {
                if (options.settingsFailure) throw new Error("Settings unavailable");
                if (init.method === "POST") return {ok:!options.postFailure,
                    text:async () => "Cambio rechazado", json:async () => ({...configuration,
                        busy:true, job:{id:1,state:"queued",message:"Cambio en cola; aun no confirmado"}})};
                return {ok:true,json:async () => configuration};
            }
            return {ok:true,json:async () => url === "/api/panel" ? panel : host};
        }
    };
    vm.runInNewContext(source, context);
    await new Promise(resolve => setImmediate(resolve));
    return {elements, cards, requests, confirmations, timers, configuration};
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

function configuration() {
    return {busy:false,job:{id:0,state:"idle",message:""}, additional:[
        {group:"house",query:"QOPPT",result:{stale:false,decoded:true,tokens:Array(24).fill("2")}},
        {group:"inverter",query:"QDI",result:{stale:true,status:"unsupported_nak",raw:"(old defaults"}}],
        settings:[
            {id:"output_priority",group:"house",label:"Prioridad de alimentacion",query:"QPIRI",value:2,editable:true,
                options:[{value:0,label:"Red primero"},{value:1,label:"Solar primero"},{value:2,label:"SBU"}]},
            {id:"absorption",group:"battery",label:"Absorcion",query:"QPIRI",value:28.2,editable:true,
                options:[],min:25,max:31.5,step:0.1},
            {id:"equalization",group:"battery",label:"Ecualizacion",query:"QBEQI",value:null,editable:false,
                options:[],status:"unsupported_nak",reason:"Solo consulta"},
            {id:"input_range",group:"grid",label:"Rango AC",query:"QPIRI",value:0,editable:true,
                options:[{value:0,label:"Electrodomesticos"},{value:1,label:"UPS"}]}
        ]};
}
function settingControls(fixture, index = 0) {
    const row = fixture.elements.get("settings-list").children[index];
    return row.children.find(c => c.className === "setting-controls")?.children;
}
test("settings are grouped by popup; ESP32 never offers inverter settings", async () => {
    const f = await fixture({}, false, {configuration:configuration()});
    f.cards.find(c => c.dataset.card === "battery").click();
    const rows = f.elements.get("settings-list").children;
    assert.equal(rows.length, 2);
    assert.equal(rows[0].children[0].textContent, "Absorcion");
    assert.match(rows[1].children[2].textContent, /unsupported_nak/);
    assert.equal(settingControls(f, 1), undefined);
    f.cards.find(c => c.dataset.card === "grid").click();
    assert.equal(f.elements.get("settings-list").children[0].children[0].textContent, "Rango AC");
    f.cards.find(c => c.dataset.card === "esp32").click();
    assert.equal(f.elements.get("settings-section").hidden, true);
    assert.equal(f.requests.filter(r => r.init.method === "POST").length, 0);
});
test("settings apply confirms and submits only the selected allowlisted ID and values", async () => {
    const f = await fixture({}, false, {configuration:configuration()});
    f.cards.find(c => c.dataset.card === "house").click();
    const [input, button] = settingControls(f); input.value = "1"; await button.onclick();
    const posts = f.requests.filter(r => r.init.method === "POST");
    assert.equal(posts.length, 1);
    assert.deepEqual(JSON.parse(posts[0].init.body), {id:"output_priority",value:1,previous:2,confirm:true});
    assert.match(f.confirmations[0], /Axpert real/);
    assert.match(f.elements.get("settings-status").textContent, /cola.*no confirmado/);
    assert.equal(settingControls(f)[1].disabled, true);
});
test("cancelled confirmations and invalid or unchanged values send no writes", async () => {
    const f = await fixture({}, false, {configuration:configuration(),confirm:false});
    f.cards.find(c => c.dataset.card === "house").click();
    const [input, button] = settingControls(f);
    for (const value of ["", "2", "99"]) { input.value = value; await button.onclick(); }
    assert.equal(f.confirmations.length, 0);
    input.value = "1"; await button.onclick();
    assert.equal(f.confirmations.length, 1);
    assert.equal(f.requests.filter(r => r.init.method === "POST").length, 0);
});
test("numeric voltage controls enforce documented boundaries and increments", async () => {
    const f = await fixture({}, false, {configuration:configuration()});
    f.cards.find(c => c.dataset.card === "battery").click();
    const [input, button] = settingControls(f);
    for (const value of ["24.9", "31.6", "28.25", "NaN"]) { input.value = value; await button.onclick(); }
    assert.equal(f.requests.filter(r => r.init.method === "POST").length, 0);
    input.value = "28.3"; await button.onclick();
    assert.equal(f.requests.filter(r => r.init.method === "POST").length, 1);
});
test("failed setting submission never reports success or retries automatically", async () => {
    const f = await fixture({}, false, {configuration:configuration(),postFailure:true});
    f.cards.find(c => c.dataset.card === "house").click();
    const [input, button] = settingControls(f); input.value = "1"; await button.onclick();
    assert.match(f.elements.get("settings-status").textContent, /No se puede confirmar.*rechazado.*no se reintentará/);
    assert.equal(f.requests.filter(r => r.init.method === "POST").length, 1);
    assert.equal(settingControls(f)[1].disabled, false);
});
test("settings fetch failure blocks edits without discarding live measurements and recovers", async () => {
    const options = {configuration:configuration(),settingsFailure:true};
    const f = await fixture({}, false, options);
    f.cards.find(c => c.dataset.card === "house").click();
    assert.match(f.elements.get("settings-status").textContent, /Settings unavailable/);
    assert.equal(f.elements.get("settings-list").children.length, 0);
    assert.equal(f.elements.get("output-power").textContent, "218 W");
    options.settingsFailure = false;
    await f.timers.shift()();
    assert.doesNotMatch(f.elements.get("settings-status").textContent, /Settings unavailable/);
    assert.equal(f.elements.get("settings-list").children.length, 1);
});
test("read-only schedules are hourly and stale defaults are not shown as current", async () => {
    const f = await fixture({}, false, {configuration:configuration()});
    f.cards.find(c => c.dataset.card === "house").click();
    const schedule = f.elements.get("settings-additional").children[0].children[1].textContent;
    assert.match(schedule, /00:00 · SBU/); assert.match(schedule, /23:00 · SBU/);
    assert.equal(schedule.split("\n").length, 24);
    f.cards.find(c => c.dataset.card === "inverter").click();
    const defaults = f.elements.get("settings-additional").children[0].children[1].textContent;
    assert.match(defaults, /Sin datos recientes.*unsupported_nak/);
    assert.doesNotMatch(defaults, /old defaults/);
});
test("normal polling preserves an unsaved selection; uncertain results stay explicit", async () => {
    const config = configuration();
    const f = await fixture({}, false, {configuration:config});
    f.cards.find(c => c.dataset.card === "house").click();
    const [input] = settingControls(f); input.value = "1";
    await f.timers.shift()();
    assert.equal(settingControls(f)[0], input);
    assert.equal(settingControls(f)[0].value, "1");
    config.job = {id:1,state:"uncertain",message:"CRC invalido; resultado desconocido. No se reintenta"};
    await f.timers.shift()();
    assert.match(f.elements.get("settings-status").textContent, /resultado desconocido/);
    assert.equal(f.elements.get("settings-status").classList.values.get("error"), true);
    assert.equal(f.requests.filter(r => r.init.method === "POST").length, 0);
});
