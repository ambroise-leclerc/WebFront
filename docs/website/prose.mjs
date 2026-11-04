// A deliberately small notation grammar. Prose never accepts raw HTML.
export const escapeHtml = value => String(value)
    .replaceAll('&', '&amp;').replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;').replaceAll('"', '&quot;');

export function inline(value) {
    const text = String(value);
    const pattern = /`([^`\n]+)`|\*([^*\n]+)\*/g;
    let result = '', end = 0;
    for (const match of text.matchAll(pattern)) {
        result += escapeHtml(text.slice(end, match.index));
        result += match[1] !== undefined
            ? `<code class="inline-code">${escapeHtml(match[1])}</code>`
            : `<em>${escapeHtml(match[2])}</em>`;
        end = match.index + match[0].length;
    }
    return result + escapeHtml(text.slice(end));
}

export function prose(value) {
    return String(value).split(/\n\s*\n/).filter(part => part.trim()).map(part => {
        const lines = part.trim().split('\n');
        if (lines.every(line => line.startsWith('- '))) {
            return `<ul class="explanation-list">${lines.map(line => `<li>${inline(line.slice(2))}</li>`).join('')}</ul>`;
        }
        return `<p>${inline(lines.join(' '))}</p>`;
    }).join('');
}

export const plain = value => String(value).replace(/`([^`\n]+)`|\*([^*\n]+)\*/g, (_, code, term) => code ?? term);
