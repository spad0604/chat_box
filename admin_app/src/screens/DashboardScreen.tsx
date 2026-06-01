import { useMemo, useState } from "react";
import { ActivityIndicator, Pressable, StyleSheet, Text, View } from "react-native";

import { fetchUsage } from "../api/client";
import type { Period } from "../api/types";
import { Card } from "../components/Card";
import { EmptyState } from "../components/EmptyState";
import { MetricCard } from "../components/MetricCard";
import { SectionHeader } from "../components/SectionHeader";
import { useAsyncData } from "../hooks/useAsyncData";
import { colors, radius, spacing } from "../theme";
import { formatDate } from "../utils/format";

const periods: Period[] = ["day", "week", "month"];

export function DashboardScreen() {
  const [period, setPeriod] = useState<Period>("day");
  const { data, loading, error } = useAsyncData(() => fetchUsage(period), [period]);

  const maxBucket = useMemo(() => {
    return Math.max(1, ...(data?.buckets.map((item) => item.user_messages) ?? [1]));
  }, [data]);

  return (
    <View style={styles.screen}>
      <View style={styles.rowBetween}>
        <SectionHeader
          title="Tổng quan"
          subtitle="Theo dõi mức sử dụng UniMate và nhịp hỏi đáp của sinh viên."
        />
        <View style={styles.segment}>
          {periods.map((item) => (
            <Pressable
              key={item}
              onPress={() => setPeriod(item)}
              style={[styles.segmentItem, period === item && styles.segmentActive]}
            >
              <Text style={[styles.segmentText, period === item && styles.segmentTextActive]}>
                {item === "day" ? "Ngày" : item === "week" ? "Tuần" : "Tháng"}
              </Text>
            </Pressable>
          ))}
        </View>
      </View>

      {loading ? <ActivityIndicator color={colors.primary} /> : null}
      {error ? <EmptyState title="Không tải được thống kê" message={error} /> : null}

      {data ? (
        <>
          <View style={styles.metrics}>
            <MetricCard label="Tổng tin nhắn" value={data.total_messages} />
            <MetricCard label="Câu hỏi sinh viên" value={data.user_messages} />
            <MetricCard label="Trả lời UniMate" value={data.assistant_messages} />
            <MetricCard label="Phiên chat" value={data.sessions} />
          </View>

          <Card>
            <Text style={styles.cardTitle}>Lượt hỏi theo {periodLabel(period).toLowerCase()}</Text>
            <View style={styles.chart}>
              {data.buckets.length === 0 ? (
                <EmptyState title="Chưa có dữ liệu" />
              ) : (
                data.buckets.slice(0, 14).reverse().map((bucket) => (
                  <View key={bucket.bucket} style={styles.barRow}>
                    <Text style={styles.barLabel}>{formatDate(bucket.bucket)}</Text>
                    <View style={styles.barTrack}>
                      <View
                        style={[
                          styles.barFill,
                          { width: `${Math.max(4, (bucket.user_messages / maxBucket) * 100)}%` }
                        ]}
                      />
                    </View>
                    <Text style={styles.barValue}>{bucket.user_messages}</Text>
                  </View>
                ))
              )}
            </View>
          </Card>
        </>
      ) : null}
    </View>
  );
}

function periodLabel(period: Period) {
  if (period === "day") return "Ngày";
  if (period === "week") return "Tuần";
  return "Tháng";
}

const styles = StyleSheet.create({
  screen: {
    gap: spacing.lg
  },
  rowBetween: {
    alignItems: "flex-start",
    flexDirection: "row",
    flexWrap: "wrap",
    gap: spacing.md,
    justifyContent: "space-between"
  },
  segment: {
    backgroundColor: colors.surfaceMuted,
    borderColor: colors.border,
    borderRadius: radius.md,
    borderWidth: 1,
    flexDirection: "row",
    padding: 3
  },
  segmentItem: {
    borderRadius: radius.sm,
    paddingHorizontal: spacing.md,
    paddingVertical: spacing.sm
  },
  segmentActive: {
    backgroundColor: colors.surface
  },
  segmentText: {
    color: colors.muted,
    fontWeight: "800"
  },
  segmentTextActive: {
    color: colors.primary
  },
  metrics: {
    flexDirection: "row",
    flexWrap: "wrap",
    gap: spacing.md
  },
  cardTitle: {
    color: colors.text,
    fontSize: 17,
    fontWeight: "900",
    marginBottom: spacing.md
  },
  chart: {
    gap: spacing.sm
  },
  barRow: {
    alignItems: "center",
    flexDirection: "row",
    gap: spacing.sm
  },
  barLabel: {
    color: colors.muted,
    fontSize: 12,
    width: 110
  },
  barTrack: {
    backgroundColor: colors.surfaceMuted,
    borderRadius: radius.sm,
    flex: 1,
    height: 12,
    overflow: "hidden"
  },
  barFill: {
    backgroundColor: colors.primary,
    height: 12
  },
  barValue: {
    color: colors.text,
    fontSize: 13,
    fontWeight: "800",
    textAlign: "right",
    width: 34
  }
});

