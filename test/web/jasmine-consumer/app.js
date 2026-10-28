document.getElementById('increment').addEventListener('click', () => {
    const value = document.getElementById('value');
    value.textContent = String(Number(value.textContent) + 1);
});
window.consumerLoaded = true;
