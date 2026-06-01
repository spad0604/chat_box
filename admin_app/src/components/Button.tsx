import { ReactNode } from "react";
import { Pressable, StyleSheet, Text, ViewStyle } from "react-native";

import { colors, radius, spacing } from "../theme";

type Props = {
  children: ReactNode;
  tone?: "primary" | "neutral" | "danger";
  disabled?: boolean;
  style?: ViewStyle;
  onPress: () => void;
};

export function Button({ children, tone = "neutral", disabled, style, onPress }: Props) {
  return (
    <Pressable
      disabled={disabled}
      onPress={onPress}
      style={({ pressed }) => [
        styles.base,
        styles[tone],
        disabled && styles.disabled,
        pressed && !disabled && styles.pressed,
        style
      ]}
    >
      <Text style={[styles.text, tone === "neutral" && styles.neutralText]}>{children}</Text>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  base: {
    alignItems: "center",
    borderRadius: radius.sm,
    minHeight: 38,
    justifyContent: "center",
    paddingHorizontal: spacing.md,
    paddingVertical: spacing.sm
  },
  primary: {
    backgroundColor: colors.primary
  },
  neutral: {
    backgroundColor: colors.surfaceMuted,
    borderColor: colors.border,
    borderWidth: 1
  },
  danger: {
    backgroundColor: colors.red
  },
  disabled: {
    opacity: 0.45
  },
  pressed: {
    opacity: 0.78
  },
  text: {
    color: "#ffffff",
    fontSize: 14,
    fontWeight: "700"
  },
  neutralText: {
    color: colors.text
  }
});

