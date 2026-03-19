import { motion, useSpring, useTransform, type MotionValue } from "motion/react";
import { type EyeConfig } from "../lib/emotions";

interface EyeProps {
  config: EyeConfig;
  mouseX: MotionValue<number>;
  mouseY: MotionValue<number>;
  isBlinking: boolean;
}

export function Eye({ config, mouseX, mouseY, isBlinking }: EyeProps) {
  const springX = useSpring(mouseX, { stiffness: 100, damping: 12, mass: 0.5 });
  const springY = useSpring(mouseY, { stiffness: 100, damping: 12, mass: 0.5 });

  // 跟随范围 ±120px
  const lookX = useTransform(springX, [-1, 1], [-120, 120]);
  const lookY = useTransform(springY, [-1, 1], [-120, 120]);

  return (
    <div style={{ position: "relative", width: 120, height: 160, display: "flex", alignItems: "center", justifyContent: "center" }}>
      <motion.div style={{ x: lookX, y: lookY, position: "absolute" }}>
        <svg width="120" height="160" viewBox="0 0 100 140" overflow="visible">
          <motion.path
            d={config.path}
            animate={{
              d: config.path,
              fill: config.color,
              scaleX: config.scaleX,
              scaleY: isBlinking ? 0.04 : config.scaleY,
              rotate: config.rotate,
              x: config.x,
              y: config.y,
            }}
            transition={
              isBlinking
                ? { duration: 0.06, ease: "easeOut" }
                : { type: "spring", stiffness: 220, damping: 14, mass: 0.7 }
            }
            style={{
              filter: `drop-shadow(0 0 10px ${config.color}dd) drop-shadow(0 0 28px ${config.color}88) drop-shadow(0 0 50px ${config.color}44)`,
              transformOrigin: "center",
            }}
          />
        </svg>
      </motion.div>
    </div>
  );
}
