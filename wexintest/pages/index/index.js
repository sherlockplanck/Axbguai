const ONENET = {
  productId: 'C21bW84s72',
  deviceName: 'data',
  token: 'version=2018-10-31&res=products%2FC21bW84s72%2Fdevices%2Fdata&et=1910080768&method=md5&sign=1HjjPj0U%2B0xJoTLkBSn1hw%3D%3D',
  baseUrl: 'https://iot-api.heclouds.com',
}

const PROPERTY = {
  longitude: 'longitude',
  latitude: 'latitude',
  fallAlarm: 'fall_alarm',
  textMsg: 'text_msg',
  guaiMessage: 'guai_message',
}

const REFRESH_INTERVAL = 3000
const LOGIN_STATE_KEY = 'smartGuaiLoggedIn'
const LAST_GUAI_MESSAGE_KEY = 'smartGuaiLastPromptMessage'

Page({
  data: {
    longitude: '--',
    latitude: '--',
    fallAlarm: '--',
    fallAlarmActive: false,
    lastUpdate: '未获取',
    loading: false,
    sending: false,
    errorMessage: '',
    textMsg: '',
  },

  onLoad() {
    if (!isLoggedIn()) {
      redirectToLogin()
      return
    }

    this.fetchDeviceData().catch(() => {})
    this.refreshTimer = setInterval(() => {
      this.fetchDeviceData().catch(() => {})
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
    this.fetchDeviceData().then(() => {
      wx.stopPullDownRefresh()
    }).catch(() => {
      wx.stopPullDownRefresh()
    })
  },

  onMessageInput(e) {
    this.setData({
      textMsg: e.detail.value,
    })
  },

  openMap() {
    wx.navigateTo({
      url: '/pages/map/map',
    })
  },

  openGuaiMessage() {
    openGuaiMessagePage()
  },

  openContacts() {
    wx.navigateTo({
      url: '/pages/contacts/contacts',
    })
  },

  checkGuaiMessage(properties) {
    if (this.messageAlertShowing) {
      return
    }

    const guaiMessage = getOptionalPropertyValue(properties, PROPERTY.guaiMessage)
    const message = guaiMessage.trim()

    if (!message) {
      wx.removeStorageSync(LAST_GUAI_MESSAGE_KEY)
      return
    }

    if (message === wx.getStorageSync(LAST_GUAI_MESSAGE_KEY)) {
      return
    }

    this.messageAlertShowing = true
    wx.showModal({
      title: '收到老人消息',
      content: message,
      cancelText: '关闭',
      confirmText: '直接打开',
      success: (res) => {
        wx.setStorageSync(LAST_GUAI_MESSAGE_KEY, message)

        if (res.confirm) {
          openGuaiMessagePage()
        }
      },
      complete: () => {
        this.messageAlertShowing = false
      },
    })
  },

  fetchDeviceData() {
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
            const message = getResponseMessage(res, '获取数据失败')
            this.setData({
              errorMessage: message,
            })
            reject(new Error(message))
            return
          }

          const properties = normalizeProperties(res.data && res.data.data)
          const rawFallAlarm = getPropertyValue(properties, PROPERTY.fallAlarm)
          this.checkGuaiMessage(properties)

          this.setData({
            longitude: getPropertyValue(properties, PROPERTY.longitude),
            latitude: getPropertyValue(properties, PROPERTY.latitude),
            fallAlarm: formatFallAlarm(rawFallAlarm),
            fallAlarmActive: isAlarmValue(rawFallAlarm),
            lastUpdate: formatClock(new Date()),
            errorMessage: '',
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

  sendMessage() {
    const message = this.data.textMsg.trim()

    if (!message) {
      wx.showToast({
        title: '请输入留言',
        icon: 'none',
      })
      return
    }

    this.setData({
      sending: true,
    })

    wx.request({
      url: `${ONENET.baseUrl}/thingmodel/set-device-property`,
      method: 'POST',
      data: {
        product_id: ONENET.productId,
        device_name: ONENET.deviceName,
        params: {
          [PROPERTY.textMsg]: message,
        },
      },
      header: {
        authorization: ONENET.token,
        'content-type': 'application/json',
      },
      success: (res) => {
        if (!isSuccessResponse(res)) {
          wx.showToast({
            title: getResponseMessage(res, '发送失败'),
            icon: 'none',
          })
          return
        }

        this.setData({
          textMsg: '',
        })
        wx.showToast({
          title: '发送成功',
          icon: 'success',
        })
      },
      fail: (err) => {
        wx.showToast({
          title: err.errMsg || '网络请求失败',
          icon: 'none',
        })
      },
      complete: () => {
        this.setData({
          sending: false,
        })
      },
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
    return '--'
  }

  return `${property.value}`
}

function getOptionalPropertyValue(properties, identifier) {
  const property = properties.find((item) => item.identifier === identifier)

  if (!property || property.value === undefined || property.value === null) {
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

function isAlarmValue(value) {
  const normalized = `${value}`.toLowerCase()
  return normalized === '1' || normalized === 'true' || normalized === 'alarm'
}

function formatFallAlarm(value) {
  if (value === '--') {
    return '--'
  }

  return isAlarmValue(value) ? '已触发' : '正常'
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

function openGuaiMessagePage() {
  const pages = getCurrentPages()
  const currentPage = pages[pages.length - 1]

  if (currentPage && currentPage.route === 'pages/guaiMessage/guaiMessage') {
    return
  }

  wx.navigateTo({
    url: '/pages/guaiMessage/guaiMessage',
  })
}
