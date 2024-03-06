using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using HarmonyLib;
using UnityEngine;

namespace MemoryMinimizer.HarmonyPatches {
	[HarmonyPatch(typeof(CustomPreviewBeatmapLevel), nameof(CustomPreviewBeatmapLevel.GetCoverImageAsync))]
	static class ScaleCustomSongCovers {
		const int MAX_CACHED_COVERS = 50;
		static readonly List<(CustomPreviewBeatmapLevel beatmapLevel, Sprite sprite)> coverCacheInvalidator = new List<(CustomPreviewBeatmapLevel, Sprite)>();

		[HarmonyPatch(typeof(StandardLevelDetailView), nameof(StandardLevelDetailView.SetContent))]
		static class HookSongSelection {
			public static IBeatmapLevel lastSelectedLevel { get; private set; }

			static void Postfix(IBeatmapLevel level) => lastSelectedLevel = level;
		}

		static bool Prefix(CustomPreviewBeatmapLevel __instance, CancellationToken cancellationToken, ref Task<Sprite> __result, ref Sprite ____coverImage) {
			if(____coverImage != null) {
				// "Refresh" the cover in the cache LIFO
				var cachedIndex = coverCacheInvalidator.FindIndex(x => x.beatmapLevel == __instance);
				if(cachedIndex != -1 && cachedIndex + 1 != coverCacheInvalidator.Count) {
					coverCacheInvalidator.Add(coverCacheInvalidator[cachedIndex]);
					coverCacheInvalidator.RemoveAt(cachedIndex);
				}

				__result = Task.FromResult(____coverImage);

				return false;
			}
			
			if(string.IsNullOrEmpty(__instance.standardLevelInfoSaveData.coverImageFilename)) {
				__result = Task.FromResult(__instance.defaultCoverImage);

				return false;
			}

			string path = Path.Combine(__instance.customLevelPath, __instance.standardLevelInfoSaveData.coverImageFilename);

			if(!File.Exists(path)) {
				__result = Task.FromResult(__instance.defaultCoverImage);

				return false;
			}

			__result = ScaleCover(path, Config.Instance.maxCoverSize, cancellationToken).ContinueWith(
				x => {
					if(!x.IsCompleted || x.IsFaulted || x.IsCanceled)
						return __instance.defaultCoverImage;

					var (imageWidth, imageHeight, imageBgra) = x.Result;

					var t = new Texture2D(imageWidth, imageHeight, TextureFormat.BGRA32, false);

					t.wrapMode = TextureWrapMode.Clamp;
					t.LoadRawTextureData(x.Result.imageBgra);
					t.Apply();

					var coverImage = Sprite.Create(t, 
						new Rect(0.0f, 0.0f, t.width, t.height), new Vector2(0.5f, 0.5f), 
						100.0f, 
						0, 
						SpriteMeshType.FullRect
					);

					IPA.Utilities.ReflectionUtil.SetField(__instance, "_coverImage", coverImage);

					coverCacheInvalidator.Add((__instance, coverImage));

					for(var i = coverCacheInvalidator.Count - MAX_CACHED_COVERS; i-- > 0;) {
						var songToInvalidate = coverCacheInvalidator[i];

						if(HookSongSelection.lastSelectedLevel == songToInvalidate.beatmapLevel)
							continue;

						coverCacheInvalidator.RemoveAt(i);

						if(songToInvalidate.beatmapLevel != null && songToInvalidate.sprite != null) {
							IPA.Utilities.ReflectionUtil.SetField<CustomPreviewBeatmapLevel, Sprite>(songToInvalidate.beatmapLevel, "_coverImage", null);

							GameObject.DestroyImmediate(songToInvalidate.sprite.texture);
							GameObject.DestroyImmediate(songToInvalidate.sprite);
						}
					}

					return coverImage;
				},
				cancellationToken, TaskContinuationOptions.None, TaskScheduler.FromCurrentSynchronizationContext()
			);

			return false;
		}

		static Task<(int imageWidth, int imageHeight, byte[] imageBgra)> ScaleCover(string path, int maxSideLength = 200, CancellationToken cancellationToken = default) {
			return Task.Run(() => {
				using(var image = Image.FromFile(path)) {
					maxSideLength = Mathf.Clamp(maxSideLength, 32, Math.Max(image.Width, image.Height));

					var ratio = (float)image.Width / image.Height;
					var destRect = new Rectangle(0, 0, maxSideLength, maxSideLength);

					if(image.Width <= image.Height) {
						destRect.Height = (int)(maxSideLength * ratio);
					} else {
						destRect.Width = (int)(maxSideLength * ratio);
					}

					if(cancellationToken.IsCancellationRequested)
						throw new TaskCanceledException();

					using(var destImage = new Bitmap(destRect.Width, destRect.Height, PixelFormat.Format32bppArgb)) {
						using(var graphics = System.Drawing.Graphics.FromImage(destImage)) {
							graphics.CompositingMode = CompositingMode.SourceCopy;

							graphics.CompositingQuality = CompositingQuality.HighSpeed;
							graphics.InterpolationMode = InterpolationMode.HighQualityBicubic;
							graphics.SmoothingMode = SmoothingMode.None;
							graphics.PixelOffsetMode = PixelOffsetMode.None;

							using(var wrapMode = new ImageAttributes()) {
								wrapMode.SetWrapMode(System.Drawing.Drawing2D.WrapMode.TileFlipXY);
								graphics.DrawImage(image, destRect, 0, 0, image.Width, image.Height, GraphicsUnit.Pixel, wrapMode);
							}
						}

						if(cancellationToken.IsCancellationRequested)
							throw new TaskCanceledException();

						destImage.RotateFlip(RotateFlipType.Rotate180FlipX);

						BitmapData bmpdata = null;

						try {
							bmpdata = destImage.LockBits(destRect, ImageLockMode.ReadOnly, destImage.PixelFormat);
							
							int numbytes = bmpdata.Stride * destImage.Height;

							var bytedata = new byte[numbytes];
							var ptr = bmpdata.Scan0;

							Marshal.Copy(ptr, bytedata, 0, numbytes);

							return (destImage.Width, destImage.Height, bytedata);
						} finally {
							if(bmpdata != null)
								destImage.UnlockBits(bmpdata);
						}
					}
				}
			}, cancellationToken);
		}
	}
}