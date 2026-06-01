import * as DocumentPicker from "expo-document-picker";
import { ActivityIndicator, Alert, Platform, ScrollView, StyleSheet, Text, View } from "react-native";

import { deleteDocument, fetchDocuments, replaceDocument, uploadDocument } from "../api/client";
import type { AdminDocument } from "../api/types";
import { Button } from "../components/Button";
import { Card } from "../components/Card";
import { EmptyState } from "../components/EmptyState";
import { SectionHeader } from "../components/SectionHeader";
import { useAsyncData } from "../hooks/useAsyncData";
import { colors, radius, spacing } from "../theme";
import { formatBytes, formatDate } from "../utils/format";

export function DocumentsScreen() {
  const documents = useAsyncData(fetchDocuments, []);

  async function handleUpload() {
    const file = await pickFile();
    if (!file) return;
    await runDocumentAction(async () => uploadDocument(file), documents.refresh);
  }

  async function handleReplace(document: AdminDocument) {
    const file = await pickFile();
    if (!file) return;
    await runDocumentAction(async () => replaceDocument(document.id, file), documents.refresh);
  }

  async function handleDelete(document: AdminDocument) {
    const confirmed = await confirmDelete(document.filename);
    if (!confirmed) return;
    await runDocumentAction(async () => deleteDocument(document.id), documents.refresh);
  }

  return (
    <View style={styles.screen}>
      <View style={styles.headerRow}>
        <SectionHeader
          title="Tài liệu"
          subtitle="Quản lý tài liệu quy chế/học vụ được gắn vào vector store để LLM tra cứu."
        />
        <Button tone="primary" onPress={handleUpload}>
          Thêm tài liệu
        </Button>
      </View>

      <Card>
        {documents.loading ? <ActivityIndicator color={colors.primary} /> : null}
        {documents.error ? <EmptyState title="Không tải được tài liệu" message={documents.error} /> : null}
        {documents.data?.length === 0 && !documents.loading ? (
          <EmptyState title="Chưa có tài liệu" message="Upload tài liệu để UniMate dùng khi trả lời câu hỏi quy chế." />
        ) : null}

        <ScrollView style={styles.list}>
          {documents.data?.map((document) => (
            <View key={document.id} style={styles.documentRow}>
              <View style={styles.docIcon}>
                <Text style={styles.docIconText}>DOC</Text>
              </View>
              <View style={styles.documentBody}>
                <Text style={styles.filename}>{document.filename}</Text>
                <Text style={styles.meta}>
                  {formatBytes(document.size)} · {document.content_type} · {formatDate(document.updated_at)}
                </Text>
                <View style={styles.statusPill}>
                  <Text style={styles.statusText}>{document.status}</Text>
                </View>
              </View>
              <View style={styles.actions}>
                <Button onPress={() => handleReplace(document)}>Sửa</Button>
                <Button tone="danger" onPress={() => handleDelete(document)}>
                  Xóa
                </Button>
              </View>
            </View>
          ))}
        </ScrollView>
      </Card>
    </View>
  );
}

async function pickFile(): Promise<File | null> {
  const result = await DocumentPicker.getDocumentAsync({
    copyToCacheDirectory: true,
    multiple: false
  });

  if (result.canceled || !result.assets[0]) {
    return null;
  }

  const asset = result.assets[0];
  const maybeFile = (asset as unknown as { file?: File }).file;
  if (maybeFile) {
    return maybeFile;
  }

  const response = await fetch(asset.uri);
  const blob = await response.blob();
  return new File([blob], asset.name, {
    type: asset.mimeType || "application/octet-stream"
  });
}

async function runDocumentAction(action: () => Promise<void>, refresh: () => Promise<void>) {
  try {
    await action();
    await refresh();
  } catch (err) {
    const message = err instanceof Error ? err.message : "Unknown error";
    if (Platform.OS === "web") {
      window.alert(message);
    } else {
      Alert.alert("Lỗi", message);
    }
  }
}

async function confirmDelete(filename: string) {
  if (Platform.OS === "web") {
    return window.confirm(`Xóa tài liệu "${filename}"?`);
  }
  return new Promise<boolean>((resolve) => {
    Alert.alert("Xóa tài liệu", `Xóa tài liệu "${filename}"?`, [
      { text: "Hủy", style: "cancel", onPress: () => resolve(false) },
      { text: "Xóa", style: "destructive", onPress: () => resolve(true) }
    ]);
  });
}

const styles = StyleSheet.create({
  screen: {
    gap: spacing.lg
  },
  headerRow: {
    alignItems: "flex-start",
    flexDirection: "row",
    flexWrap: "wrap",
    gap: spacing.md,
    justifyContent: "space-between"
  },
  list: {
    maxHeight: 680
  },
  documentRow: {
    alignItems: "center",
    borderBottomColor: colors.border,
    borderBottomWidth: 1,
    flexDirection: "row",
    flexWrap: "wrap",
    gap: spacing.md,
    paddingVertical: spacing.md
  },
  docIcon: {
    alignItems: "center",
    backgroundColor: colors.primarySoft,
    borderRadius: radius.md,
    height: 48,
    justifyContent: "center",
    width: 48
  },
  docIconText: {
    color: colors.primary,
    fontSize: 11,
    fontWeight: "900"
  },
  documentBody: {
    flex: 1,
    gap: spacing.xs,
    minWidth: 260
  },
  filename: {
    color: colors.text,
    fontSize: 15,
    fontWeight: "900"
  },
  meta: {
    color: colors.muted,
    fontSize: 12
  },
  statusPill: {
    alignSelf: "flex-start",
    backgroundColor: colors.surfaceMuted,
    borderColor: colors.border,
    borderRadius: 999,
    borderWidth: 1,
    paddingHorizontal: spacing.sm,
    paddingVertical: 4
  },
  statusText: {
    color: colors.green,
    fontSize: 12,
    fontWeight: "900"
  },
  actions: {
    flexDirection: "row",
    gap: spacing.sm
  }
});

