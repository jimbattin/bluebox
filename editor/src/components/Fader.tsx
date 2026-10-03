import type { Param } from "../programs";

interface Props {
  id: string;
  label: string;
  readout: string;
  caption: string;
  value: number;
  max: number;
  disabled: boolean;
  help?: Param | { name: string; help: string };
  onInput: (v: number) => void;
  /** dragging = true on pointer-down, false on release; called on each input as well. */
  onTouch: (key: string, dragging: boolean) => void;
  onHelp: (text: string | null) => void;
}

export function Fader({ id, label, readout, caption, value, max, disabled, help, onInput, onTouch, onHelp }: Props) {
  const text = help ? `${help.name} — ${help.help}` : null;
  return (
    <div class={`fader${disabled ? " empty" : ""}`}>
      <div class="led-label">{label}</div>
      <div class="led-value">{readout}</div>
      <input
        type="range"
        min={0}
        max={max}
        step={1}
        value={value}
        disabled={disabled}
        aria-label={caption}
        onPointerDown={() => {
          onTouch(id, true);
          onHelp(text);
        }}
        onPointerUp={() => onTouch(id, false)}
        onPointerCancel={() => onTouch(id, false)}
        onPointerEnter={() => onHelp(text)}
        onPointerLeave={() => onHelp(null)}
        onFocus={() => onHelp(text)}
        onBlur={() => onHelp(null)}
        onInput={(e) => {
          onTouch(id, true);
          onInput(Number(e.currentTarget.value));
        }}
        onChange={() => onTouch(id, false)}
      />
      <div class="silk caption">{caption}</div>
    </div>
  );
}
