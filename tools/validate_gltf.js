// glTF conformance gate (file-io spec): runs the official Khronos
// gltf-validator over a .glb and fails on any error.
//   npm install gltf-validator && node tools/validate_gltf.js <file.glb>

const validator = require('gltf-validator');
const fs = require('fs');

const path = process.argv[2];
if (!path) {
    console.error('usage: node validate_gltf.js <file.glb>');
    process.exit(2);
}

validator
    .validateBytes(new Uint8Array(fs.readFileSync(path)))
    .then((report) => {
        const i = report.issues;
        console.log(`gltf-validator: ${path}: ${i.numErrors} errors, ${i.numWarnings} warnings`);
        for (const m of i.messages) {
            console.log(`  [${m.severity}] ${m.code}: ${m.message} @ ${m.pointer}`);
        }
        process.exit(i.numErrors > 0 ? 1 : 0);
    })
    .catch((err) => {
        console.error('gltf-validator failed to run:', err);
        process.exit(2);
    });
