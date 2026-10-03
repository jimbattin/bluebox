import type { LinkSnapshot } from "../midi";

interface Props {
  link: LinkSnapshot;
  onInput: (id: string) => void;
  onOutput: (id: string) => void;
}

export function Header({ link, onInput, onOutput }: Props) {
  return (
    <header class="plate">
      <div class="brand">
        <span class="wordmark">
          <span class="wordmark-blue">BLUE</span>BOX
        </span>
        <span class="badge">BARC</span>
      </div>
      <div class="link">
        <span class={`led ${link.linked ? "green" : "on"}`} />
        <span class="link-status">{link.status}</span>
      </div>
      <label class="port">
        <span>MIDI IN</span>
        <select value={link.inputId} onChange={(e) => onInput(e.currentTarget.value)}>
          {link.inputs.length === 0 && <option value="">—</option>}
          {link.inputs.map((p) => (
            <option key={p.id} value={p.id}>
              {p.name}
            </option>
          ))}
        </select>
      </label>
      <label class="port">
        <span>MIDI OUT</span>
        <select value={link.outputId} onChange={(e) => onOutput(e.currentTarget.value)}>
          {link.outputs.length === 0 && <option value="">—</option>}
          {link.outputs.map((p) => (
            <option key={p.id} value={p.id}>
              {p.name}
            </option>
          ))}
        </select>
      </label>
    </header>
  );
}
