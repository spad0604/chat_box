import { StyleSheet, Text, View } from "react-native";

import { colors, spacing } from "../theme";
import { Card } from "./Card";

type Props = {
  label: string;
  value: number | string;
};

export function MetricCard({ label, value }: Props) {
  return (
    <Card style={styles.card}>
      <Text style={styles.value}>{value}</Text>
      <Text style={styles.label}>{label}</Text>
    </Card>
  );
}

const styles = StyleSheet.create({
  card: {
    flexBasis: 180,
    flexGrow: 1,
    gap: spacing.xs
  },
  value: {
    color: colors.text,
    fontSize: 30,
    fontWeight: "900"
  },
  label: {
    color: colors.muted,
    fontSize: 13,
    fontWeight: "700"
  }
});

