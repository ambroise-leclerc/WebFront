const notes = [
    'Loaded through <script type="module">',
    'Imported from ./messages.mjs',
    'Resolved relative to the importing module'
];

export function importedModuleNote() {
    return notes.join(' | ');
}
