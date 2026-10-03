declare module "*.html" {
  const bundle: import("bun").HTMLBundle;
  export default bundle;
}
