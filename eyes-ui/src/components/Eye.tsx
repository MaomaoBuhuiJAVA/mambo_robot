import { motion, useSpring, useTransform, type MotionValue } from "motion/react";
import { type EyeConfig } from "../lib/emotions";

interface EyeProps {
  config: EyeConfig;
  mouseX: MotionValue<number>;
  mouseY: MotionValue<number>;
  isBlinking: boolean;
  isDizzy?: boolean;
}

export function Eye({ config, mouseX, mouseY, isBlinking, isDizzy }: EyeProps) {
  const springX = useSpring(mouseX, { stiffness: 400, damping: 30, mass: 0.3 });
  const springY = useSpring(mouseY, { stiffness: 400, damping: 30, mass: 0.3 });

  const lookX = useTransform(springX, [-1, 1], isDizzy ? [0, 0] : [-120, 120]);
  const lookY = useTransform(springY, [-1, 1], isDizzy ? [0, 0] : [-120, 120]);

  return (
    <div style={{ position: "relative", width: 120, height: 160, display: "flex", alignItems: "center", justifyContent: "center" }}>
      <motion.div style={{ x: lookX, y: lookY, position: "absolute" }}>
        <svg width="120" height="160" viewBox="0 0 100 140" overflow="visible">
          {isDizzy ? (
            <motion.g
              animate={{ rotate: 360 }}
              transition={{ duration: 1.2, repeat: Infinity, ease: "linear" }}
              style={{ transformOrigin: "50px 70px" }}
            >
              <motion.circle
                cx="50" cy="70" r="38"
                fill="none"
                stroke={config.color}
                strokeWidth="10"
                strokeDasharray="60 80"
                animate={{ opacity: [1, 0.4, 1] }}
                transition={{ duration: 0.8, repeat: Infinity, ease: "easeInOut" }}
                style={{ filter: `drop-shadow(0 0 10px ${config.color}dd) drop-shadow(0 0 28px ${config.color}88)` }}
              />
              <motion.circle
                cx="50" cy="70" r="20"
                fill="none"
                stroke={config.color}
                strokeWidth="8"
                strokeDasharray="30 45"
                animate={{ opacity: [0.4, 1, 0.4] }}
                transition={{ duration: 0.8, repeat: Infinity, ease: "easeInOut" }}
                style={{ filter: `drop-shadow(0 0 8px ${config.color}cc)` }}
              />
              <circle cx="50" cy="70" r="6" fill={config.color}
                style={{ filter: `drop-shadow(0 0 6px ${config.color})` }} />
            </motion.g>
          ) : (
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
          )}
        </svg>
      </motion.div>
    </div>
  );
}
