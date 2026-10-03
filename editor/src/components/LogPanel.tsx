import { useState } from "preact/hooks";

interface Props {
  lines: string[];
}

export function LogPanel({ lines }: Props) {
  const [open, setOpen] = useState(false);
  return (
    <section class="log">
      <button class="log-toggle" onClick={() => setOpen(!open)}>
        {open ? "▾" : "▸"} LOG ({lines.length})
      </button>
      {open && <pre class="log-body">{lines.join("\n")}</pre>}
    </section>
  );
}
