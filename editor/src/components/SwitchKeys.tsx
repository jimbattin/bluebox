export interface SwitchKey {
  id: string;
  label: string;
  lit: boolean;
  help: string;
  onPress: () => void;
}

interface Props {
  keys: SwitchKey[];
  disabled: boolean;
  onHelp: (text: string | null) => void;
  className?: string;
}

export function SwitchKeys({ keys, disabled, onHelp, className }: Props) {
  return (
    <div class={`keys switch-keys${className ? " " + className : ""}`}>
      {keys.map((k) => (
        <button
          key={k.id}
          class={`key${k.lit ? " lit" : ""}`}
          disabled={disabled}
          onClick={k.onPress}
          onPointerEnter={() => onHelp(k.help)}
          onPointerLeave={() => onHelp(null)}
          onFocus={() => onHelp(k.help)}
          onBlur={() => onHelp(null)}
        >
          <span class="led" />
          <span class="key-label">{k.label}</span>
        </button>
      ))}
    </div>
  );
}
