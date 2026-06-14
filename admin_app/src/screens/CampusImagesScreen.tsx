import * as DocumentPicker from "expo-document-picker";
import { ActivityIndicator, Alert, Image, Platform, ScrollView, StyleSheet, Text, View } from "react-native";

import { deleteCampusImage, fetchCampusImages, uploadCampusImage } from "../api/client";
import type { CampusImage } from "../api/types";
import { Button } from "../components/Button";
import { Card } from "../components/Card";
import { EmptyState } from "../components/EmptyState";
import { SectionHeader } from "../components/SectionHeader";
import { useAsyncData } from "../hooks/useAsyncData";
import { colors, radius, spacing } from "../theme";

const maxImageBytes = 512 * 1024;

export function CampusImagesScreen() {
  const images = useAsyncData(fetchCampusImages, []);

  async function handleUpload() {
    const file = await pickImage();
    if (!file) return;
    await runCampusImageAction(async () => uploadCampusImage(file), images.refresh);
  }

  async function handleDelete(image: CampusImage) {
    const confirmed = await confirmDelete(image.filename);
    if (!confirmed) return;
    await runCampusImageAction(async () => deleteCampusImage(image.filename), images.refresh);
  }

  return (
    <View style={styles.screen}>
      <View style={styles.headerRow}>
        <SectionHeader
          title="Ảnh campus"
          subtitle="Quản lý ảnh hiển thị campus. Backend nhận ảnh tối đa 512KB và lưu vào danh sách campus."
        />
        <Button tone="primary" onPress={handleUpload}>
          Thêm ảnh
        </Button>
      </View>

      <Card>
        {images.loading ? <ActivityIndicator color={colors.primary} /> : null}
        {images.error ? <EmptyState title="Không tải được ảnh campus" message={images.error} /> : null}
        {images.data?.length === 0 && !images.loading ? (
          <EmptyState title="Chưa có ảnh campus" message="Upload ảnh để app có nội dung campus mới nhất." />
        ) : null}

        <ScrollView style={styles.list}>
          <View style={styles.grid}>
            {images.data?.map((image) => (
              <View key={image.filename} style={styles.imageCard}>
                <Image source={{ uri: image.url }} style={styles.preview} resizeMode="cover" />
                <View style={styles.imageBody}>
                  <Text style={styles.filename} numberOfLines={2}>
                    {image.filename}
                  </Text>
                  <Text style={styles.url} numberOfLines={1}>
                    {image.url}
                  </Text>
                  <View style={styles.actions}>
                    <Button tone="danger" onPress={() => handleDelete(image)}>
                      Xóa ảnh
                    </Button>
                  </View>
                </View>
              </View>
            ))}
          </View>
        </ScrollView>
      </Card>
    </View>
  );
}

async function pickImage(): Promise<File | null> {
  const result = await DocumentPicker.getDocumentAsync({
    copyToCacheDirectory: true,
    multiple: false,
    type: "image/*"
  });

  if (result.canceled || !result.assets[0]) {
    return null;
  }

  const asset = result.assets[0];
  if (asset.size && asset.size > maxImageBytes) {
    await showMessage("Ảnh quá lớn", "Ảnh campus tối đa 512KB. Vui lòng chọn ảnh nhẹ hơn.");
    return null;
  }

  const maybeFile = (asset as unknown as { file?: File }).file;
  if (maybeFile) {
    return maybeFile;
  }

  const response = await fetch(asset.uri);
  const blob = await response.blob();
  return new File([blob], asset.name, {
    type: asset.mimeType || "image/jpeg"
  });
}

async function runCampusImageAction(action: () => Promise<void>, refresh: () => Promise<void>) {
  try {
    await action();
    await refresh();
  } catch (err) {
    const message = err instanceof Error ? err.message : "Unknown error";
    await showMessage("Lỗi", message);
  }
}

async function confirmDelete(filename: string) {
  if (Platform.OS === "web") {
    return window.confirm(`Xóa ảnh campus "${filename}"?`);
  }
  return new Promise<boolean>((resolve) => {
    Alert.alert("Xóa ảnh campus", `Xóa ảnh "${filename}"?`, [
      { text: "Hủy", style: "cancel", onPress: () => resolve(false) },
      { text: "Xóa", style: "destructive", onPress: () => resolve(true) }
    ]);
  });
}

async function showMessage(title: string, message: string) {
  if (Platform.OS === "web") {
    window.alert(message);
    return;
  }
  Alert.alert(title, message);
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
    maxHeight: 720
  },
  grid: {
    flexDirection: "row",
    flexWrap: "wrap",
    gap: spacing.md
  },
  imageCard: {
    backgroundColor: colors.surfaceMuted,
    borderColor: colors.border,
    borderRadius: radius.md,
    borderWidth: 1,
    overflow: "hidden",
    width: Platform.OS === "web" ? 260 : "100%"
  },
  preview: {
    backgroundColor: colors.border,
    height: 160,
    width: "100%"
  },
  imageBody: {
    gap: spacing.xs,
    padding: spacing.md
  },
  filename: {
    color: colors.text,
    fontSize: 14,
    fontWeight: "900"
  },
  url: {
    color: colors.muted,
    fontSize: 11
  },
  actions: {
    alignItems: "flex-start",
    marginTop: spacing.sm
  }
});
