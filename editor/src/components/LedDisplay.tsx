interface Props {
  line1: string;
  line2: string;
  help: string;
}

export function LedDisplay({ line1, line2, help }: Props) {
  return (
    <section class="display-wrap">
      <div class="display">
        <div class="display-line">{line1 || "\u00a0"}</div>
        <div class="display-line">{line2 || "\u00a0"}</div>
      </div>
      <div class="silk help">{help || "\u00a0"}</div>
    </section>
  );
}
