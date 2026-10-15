const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const source = fs.readFileSync(path.join(__dirname, "..", "main", "wifi.js"), "utf8");
const html = fs.readFileSync(path.join(__dirname, "..", "main", "wifi.html"), "utf8");

async function fixture({confirmReset = true, failSave = false, failScan = false, mode = "sta"} = {}) {
    const elements = new Map();
    function element() {
        return {value:"", textContent:"", dataset:{}, children:[], disabled:false, hidden:false,
            attributes:{}, type:"password",
            setAttribute(name, value) { this.attributes[name] = value; },
            addEventListener(name, callback) { this[name] = callback; },
            append(item) { this.children.push(item); },
            replaceChildren(...items) { this.children = items; },
            reportValidity() { return true; }};
    }
    for (const match of html.matchAll(/\bid="([^"]+)"/g)) elements.set(match[1], element());
    elements.get("mode").value = "sta";
    const requests = [];
    vm.runInNewContext(source, {
        document:{getElementById(id) {
            assert.ok(elements.has(id), "Unknown Wi-Fi HTML id: " + id);
            return elements.get(id);
        }, createElement:element},
        AbortSignal, TextEncoder, confirm:() => confirmReset,
        fetch:async (url, options) => {
            requests.push({url, options});
            const failure = (failSave && options.method === "POST") || (failScan && url.endsWith("/scan"));
            return {ok:!failure, text:async () => failure ? "Error de prueba" : "Guardado. Reiniciando.",
                json:async () => url === "/api/status" ? {network_mode:mode, wifi:"192.168.4.1"} :
                    [{ssid:"<script>test</script>", rssi:-42, secured:true}]};
        }
    });
    await new Promise(resolve => setImmediate(resolve));
    return {elements, requests};
}
function fill(elements, mode = "sta", password = "") {
    elements.get("mode").value = mode;
    elements.get("mode").change();
    elements.get("ssid").value = "TestNetwork";
    elements.get("password").value = password;
}
const submit = elements => elements.get("wifi-form").submit({preventDefault() {}});

test("AP mode offers protected own network and hides station scan", async () => {
    const {elements} = await fixture({mode:"ap"});
    assert.match(elements.get("network-status").textContent, /Red propia/);
    fill(elements, "ap", "test-pass");
    assert.equal(elements.get("scan-area").hidden, true);
    assert.equal(elements.get("password").required, true);
    assert.equal(elements.get("password").maxLength, 63);
    fill(elements);
    assert.equal(elements.get("scan-area").hidden, false);
    assert.equal(elements.get("password").required, false);
});
test("network scan renders SSIDs as text and supports selection", async () => {
    const {elements} = await fixture();
    await elements.get("scan").click();
    assert.equal(elements.get("scan").disabled, false);
    assert.equal(elements.get("networks").children[1].value, "<script>test</script>");
    elements.get("networks").value = "<script>test</script>";
    elements.get("networks").change();
    assert.equal(elements.get("ssid").value, "<script>test</script>");
});
test("scan errors are visible and permit retry", async () => {
    const {elements} = await fixture({failScan:true});
    await elements.get("scan").click();
    assert.equal(elements.get("result").dataset.error, "true");
    assert.equal(elements.get("scan").disabled, false);
});
test("open station network preserves web password without sending stored secrets", async () => {
    const {elements, requests} = await fixture();
    fill(elements);
    await submit(elements);
    const post = requests.find(request => request.options.method === "POST");
    assert.equal(post.url, "/api/wifi");
    assert.equal(post.options.headers["Content-Type"], "application/json");
    assert.deepEqual(JSON.parse(post.options.body), {mode:"sta", ssid:"TestNetwork", password:"", web_password:""});
    assert.equal(elements.get("save").disabled, true);
});
test("AP password and new web password are submitted then cleared", async () => {
    const {elements, requests} = await fixture();
    fill(elements, "ap", "test-pass");
    elements.get("web-password").value = "new-web-pass";
    await submit(elements);
    const body = JSON.parse(requests.find(request => request.options.method === "POST").options.body);
    assert.equal(body.mode, "ap");
    assert.equal(body.password, "test-pass");
    assert.equal(body.web_password, "new-web-pass");
    assert.equal(elements.get("password").value, "");
    assert.equal(elements.get("web-password").value, "");
});
test("password reveal controls toggle each field independently and hide again after saving", async () => {
    const {elements} = await fixture();
    fill(elements, "ap", "test-pass");
    elements.get("web-password").value = "test-web-pass";
    elements.get("show-password").click();
    assert.equal(elements.get("password").type, "text");
    assert.equal(elements.get("web-password").type, "password");
    assert.equal(elements.get("show-password").attributes["aria-pressed"], "true");
    elements.get("show-password").click();
    assert.equal(elements.get("password").type, "password");
    elements.get("show-web-password").click();
    assert.equal(elements.get("web-password").type, "text");
    await submit(elements);
    assert.equal(elements.get("password").type, "password");
    assert.equal(elements.get("web-password").type, "password");
    assert.equal(elements.get("show-web-password").attributes["aria-pressed"], "false");
});
test("rejects invalid password lengths and oversized UTF-8 SSIDs", async () => {
    for (const [mode, password, ssid] of [
        ["ap", "", "Test"], ["ap", "x".repeat(64), "Test"],
        ["sta", "short", "Test"], ["sta", "z".repeat(64), "Test"],
        ["sta", "", "é".repeat(17)]
    ]) {
        const {elements, requests} = await fixture();
        fill(elements, mode, password);
        elements.get("ssid").value = ssid;
        await submit(elements);
        assert.equal(requests.some(request => request.options.method === "POST"), false);
        assert.equal(elements.get("result").dataset.error, "true");
    }
});
test("64 hexadecimal bytes remain supported for station PSK", async () => {
    const {elements, requests} = await fixture();
    fill(elements, "sta", "a".repeat(64));
    await submit(elements);
    assert.equal(requests.some(request => request.options.method === "POST"), true);
});
test("save failure never claims success and re-enables controls", async () => {
    const {elements} = await fixture({failSave:true});
    fill(elements, "ap", "test-pass");
    await submit(elements);
    assert.match(elements.get("result").textContent, /No se confirmó/);
    assert.equal(elements.get("save").disabled, false);
    assert.equal(elements.get("reset").disabled, false);
    assert.equal(elements.get("scan").disabled, false);
});
test("reset is cancelled without a request or explicitly confirmed as JSON", async () => {
    for (const confirmReset of [false, true]) {
        const {elements, requests} = await fixture({confirmReset});
        await elements.get("reset").click();
        const post = requests.find(request => request.options.method === "POST");
        assert.equal(Boolean(post), confirmReset);
        if (post) {
            assert.equal(post.url, "/api/wifi/reset");
            assert.deepEqual(JSON.parse(post.options.body), {confirm:"RESET"});
        }
    }
});
