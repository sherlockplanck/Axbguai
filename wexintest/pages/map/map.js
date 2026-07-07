const ONENET = {
  productId: 'C21bW84s72',
  deviceName: 'data',
  token: 'version=2018-10-31&res=products%2FC21bW84s72%2Fdevices%2Fdata&et=1910080768&method=md5&sign=1HjjPj0U%2B0xJoTLkBSn1hw%3D%3D',
  baseUrl: 'https://iot-api.heclouds.com',
}

const PROPERTY = {
  longitude: 'longitude',
  latitude: 'latitude',
}

const REFRESH_INTERVAL = 3000
const TRACK_WINDOW = 10 * 60 * 1000
const TRACK_STORAGE_KEY = 'smartGuaiTrackPoints'
const LOGIN_STATE_KEY = 'smartGuaiLoggedIn'
const DEFAULT_CENTER = {
  latitude: 39.908823,
  longitude: 116.39747,
}

Page({
  data: {
    centerLatitude: DEFAULT_CENTER.latitude,
    centerLongitude: DEFAULT_CENTER.longitude,
    currentLatitude: '--',
    currentLongitude: '--',
    hasLocation: false,
    scale: 16,
    markers: [],
    polyline: [],
    trackPoints: [],
    hasTrackPoints: false,
    lastUpdate: '未获取',
    loading: false,
    errorMessage: '',
  },

  onLoad() {
    if (!isLoggedIn()) {
      redirectToLogin()
      return
    }

    this.restoreTrackPoints()
    this.fetchLocation().catch(() => {})
    this.refreshTimer = setInterval(() => {
      this.fetchLocation().catch(() => {})
    }, REFRESH_INTERVAL)
  },

  onShow() {
    if (!isLoggedIn()) {
      this.stopRefresh()
      redirectToLogin()
    }
  },

  onUnload() {
    this.stopRefresh()
  },

  stopRefresh() {
    if (this.refreshTimer) {
      clearInterval(this.refreshTimer)
      this.refreshTimer = null
    }
  },

  onPullDownRefresh() {
    this.fetchLocation().then(() => {
      wx.stopPullDownRefresh()
    }).catch(() => {
      wx.stopPullDownRefresh()
    })
  },

  restoreTrackPoints() {
    const storedPoints = wx.getStorageSync(TRACK_STORAGE_KEY) || []
    const points = filterRecentPoints(storedPoints)
    this.renderTrack(points)
  },

  fetchLocation() {
    if (this.data.loading) {
      return Promise.resolve()
    }

    this.setData({
      loading: true,
    })

    return new Promise((resolve, reject) => {
      wx.request({
        url: `${ONENET.baseUrl}/thingmodel/query-device-property`,
        method: 'GET',
        data: {
          product_id: ONENET.productId,
          device_name: ONENET.deviceName,
        },
        header: {
          authorization: ONENET.token,
        },
        success: (res) => {
          if (!isSuccessResponse(res)) {
            const message = getResponseMessage(res, '获取位置失败')
            this.setData({
              errorMessage: message,
            })
            reject(new Error(message))
            return
          }

          const properties = normalizeProperties(res.data && res.data.data)
          const longitude = toCoordinate(getPropertyValue(properties, PROPERTY.longitude))
          const latitude = toCoordinate(getPropertyValue(properties, PROPERTY.latitude))

          if (!isValidCoordinate(latitude, longitude)) {
            const message = '经纬度数据无效'
            this.setData({
              errorMessage: message,
            })
            reject(new Error(message))
            return
          }

          const point = {
            id: `${Date.now()}`,
            latitude,
            longitude,
            time: Date.now(),
            timeText: formatClock(new Date()),
          }
          const points = mergeTrackPoint(this.data.trackPoints, point)

          wx.setStorageSync(TRACK_STORAGE_KEY, points)
          this.renderTrack(points)
          this.setData({
            errorMessage: '',
            lastUpdate: point.timeText,
          })
          resolve(res)
        },
        fail: (err) => {
          const message = err.errMsg || '网络请求失败'
          this.setData({
            errorMessage: message,
          })
          reject(err)
        },
        complete: () => {
          this.setData({
            loading: false,
          })
        },
      })
    })
  },

  renderTrack(points) {
    const latestPoint = points[points.length - 1]

    if (!latestPoint) {
      this.setData({
        trackPoints: [],
        hasTrackPoints: false,
        markers: [],
        polyline: [],
      })
      return
    }

    this.setData({
      centerLatitude: latestPoint.latitude,
      centerLongitude: latestPoint.longitude,
      currentLatitude: formatCoordinate(latestPoint.latitude),
      currentLongitude: formatCoordinate(latestPoint.longitude),
      hasLocation: true,
      trackPoints: points,
      hasTrackPoints: points.length > 0,
      markers: buildMarkers(points),
      polyline: buildPolyline(points),
    })
  },
})

function normalizeProperties(data) {
  if (!Array.isArray(data)) {
    return []
  }

  return data
}

function getPropertyValue(properties, identifier) {
  const property = properties.find((item) => item.identifier === identifier)

  if (!property || property.value === undefined || property.value === null || property.value === '') {
    return ''
  }

  return `${property.value}`
}

function isSuccessResponse(res) {
  if (res.statusCode < 200 || res.statusCode >= 300) {
    return false
  }

  return !res.data || res.data.code === 0 || res.data.code === 200
}

function getResponseMessage(res, fallback) {
  if (res.data && res.data.msg) {
    return res.data.msg
  }

  if (res.data && res.data.message) {
    return res.data.message
  }

  return fallback
}

function toCoordinate(value) {
  return Number(value)
}

function isValidCoordinate(latitude, longitude) {
  return Number.isFinite(latitude)
    && Number.isFinite(longitude)
    && latitude >= -90
    && latitude <= 90
    && longitude >= -180
    && longitude <= 180
}

function mergeTrackPoint(points, point) {
  const recentPoints = filterRecentPoints(points)
  const lastPoint = recentPoints[recentPoints.length - 1]

  if (lastPoint && isSameLocation(lastPoint, point)) {
    recentPoints[recentPoints.length - 1] = point
    return recentPoints
  }

  recentPoints.push(point)
  return recentPoints.slice(-2)
}

function filterRecentPoints(points) {
  const now = Date.now()

  if (!Array.isArray(points)) {
    return []
  }

  return points
    .filter((point) => now - point.time <= TRACK_WINDOW)
    .filter((point) => isValidCoordinate(point.latitude, point.longitude))
    .slice(-2)
}

function isSameLocation(a, b) {
  return Math.abs(a.latitude - b.latitude) < 0.000001
    && Math.abs(a.longitude - b.longitude) < 0.000001
}

function buildMarkers(points) {
  return points.map((point, index) => ({
    id: index + 1,
    latitude: point.latitude,
    longitude: point.longitude,
    width: 28,
    height: 28,
    callout: {
      content: index === points.length - 1 ? '当前位置' : '上一个位置',
      display: 'BYCLICK',
      padding: 6,
      borderRadius: 4,
    },
  }))
}

function buildPolyline(points) {
  if (points.length < 2) {
    return []
  }

  return [{
    points: points.map((point) => ({
      latitude: point.latitude,
      longitude: point.longitude,
    })),
    color: '#1f7a8c',
    width: 5,
    arrowLine: true,
  }]
}

function formatCoordinate(value) {
  return Number(value).toFixed(6)
}

function formatClock(date) {
  const hours = padNumber(date.getHours())
  const minutes = padNumber(date.getMinutes())
  const seconds = padNumber(date.getSeconds())
  return `${hours}:${minutes}:${seconds}`
}

function padNumber(value) {
  return value < 10 ? `0${value}` : `${value}`
}

function isLoggedIn() {
  return wx.getStorageSync(LOGIN_STATE_KEY) === true
}

function redirectToLogin() {
  wx.redirectTo({
    url: '/pages/login/login',
  })
}
