/// @brief Native module for the WebFront greeter example. Enables the form once the bridge is
/// ready, then awaits the Promise returned by the C++ `greet` callback.

const status = document.getElementById("status");
const form = document.getElementById("greeter-form");
const input = document.getElementById("name");
const button = document.getElementById("greet");
const result = document.getElementById("result");

function show(message, state) {
    result.textContent = message;
    result.dataset.state = state;
}

try {
    await webFront.ready;
} catch (error) {
    status.textContent = `Connection failed: ${error.message}`;
    throw error;
}

const greet = webFront.cppFunction("greet");
status.textContent = "Connected to WebFront";
input.disabled = false;
button.disabled = false;
input.focus();

form.addEventListener("submit", async (event) => {
    event.preventDefault();
    const name = input.value.trim();
    if (name === "") {
        show("Please enter your name.", "error");
        return;
    }
    try {
        show(await greet(name), "ok");
    } catch (error) {
        show(`The greeting failed: ${error.message}`, "error");
    }
});
