import { StyleSheet, Text, View } from "react-native";

import { colors, spacing } from "../theme";

type Props = {
  title: string;
  message?: string;
};

export function EmptyState({ title, message }: Props) {
  return (
    <View style={styles.wrap}>
      <Text style={styles.title}>{title}</Text>
      {message ? <Text style={styles.message}>{message}</Text> : null}
    </View>
  );
}

const styles = StyleSheet.create({
  wrap: {
    alignItems: "center",
    gap: spacing.xs,
    padding: spacing.xl
  },
  title: {
    color: colors.text,
    fontSize: 16,
    fontWeight: "800"
  },
  message: {
    color: colors.muted,
    fontSize: 14,
    textAlign: "center"
  }
});

