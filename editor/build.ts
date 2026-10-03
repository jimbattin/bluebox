export {};
const result = await Bun.build({
  entrypoints: ["./src/editor.html"],
  compile: true,
  target: "browser",
  minify: true,
});
const output = result.outputs[0];
if (!result.success || result.outputs.length !== 1 || !output) {
  for (const log of result.logs) console.error(log);
  console.error(`build failed: success=${result.success}, outputs=${result.outputs.length} (expected 1)`);
  process.exit(1);
}
await Bun.write("dist/editor.html", await output.text());
console.log("wrote dist/editor.html");
