// Writes outside permissions.publish of webfront.project.json: wf must refuse (`wf run outside`).
export default async function (context) {
    await context.project.write('elsewhere/file.txt', 'not allowed');
}
