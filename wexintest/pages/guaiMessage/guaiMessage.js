const ONENET = {
  productId: 'C21bW84s72',
  deviceName: 'data',
  token: 'version=2018-10-31&res=products%2FC21bW84s72%2Fdevices%2Fdata&et=1910080768&method=md5&sign=1HjjPj0U%2B0xJoTLkBSn1hw%3D%3D',
  baseUrl: 'https://iot-api.heclouds.com',
}

const PROPERTY = {
  guaiMessage: 'guai_message',
}

const LOGIN_STATE_KEY = 'smartGuaiLoggedIn'

Page({
  data: {
    guaiMessage: '',
    messageLength: 0,
    isEmpty: true,
    lastUpdate: '未获取',
    loading: false,
    clearing: false,
    errorMessage: '',
  },

  onLoad() {
    if (!isLoggedIn()) {
      redirectToLogin()
      return
    }

    this.fetchGuaiMessage().catch(() => {})
  },

  onShow() {
    if (!isLoggedIn()) {
      redirectToLogin()
    }
  },

  onPullDownRefresh() {
    this.fetchGuaiMessage().then(() => {
      wx.stopPullDownRefresh()
    }).catch(() => {
      wx.stopPullDownRefresh()
    })
  },

  fetchGuaiMessage() {
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
            const message = getResponseMessage(res, '读取云端消息失败')
            this.setData({
              errorMessage: message,
            })
            reject(new Error(message))
            return
          }

          const properties = normalizeProperties(res.data && res.data.data)
          const guaiMessage = getPropertyValue(properties, PROPERTY.guaiMessage)

          this.setMessageData(guaiMessage)
          this.setData({
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

  confirmClearMessage() {
    wx.showModal({
      title: '清空云端消息',
      content: '确定要清空 guai_message 吗？',
      confirmText: '清空',
      confirmColor: '#b42318',
      success: (res) => {
        if (res.confirm) {
          this.clearGuaiMessage()
        }
      },
    })
  },

  clearGuaiMessage() {
    if (this.data.clearing) {
      return
    }

    this.setData({
      clearing: true,
    })

    wx.request({
      url: `${ONENET.baseUrl}/thingmodel/set-device-property`,
      method: 'POST',
      data: {
        product_id: ONENET.productId,
        device_name: ONENET.deviceName,
        params: {
          [PROPERTY.guaiMessage]: '',
        },
      },
      header: {
        authorization: ONENET.token,
        'content-type': 'application/json',
      },
      success: (res) => {
        if (!isSuccessResponse(res)) {
          wx.showToast({
            title: getResponseMessage(res, '清空失败'),
            icon: 'none',
          })
          return
        }

        this.setMessageData('')
        this.setData({
          lastUpdate: formatClock(new Date()),
          errorMessage: '',
        })
        wx.showToast({
          title: '已清空',
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
          clearing: false,
        })
      },
    })
  },

  setMessageData(message) {
    const guaiMessage = `${message || ''}`

    this.setData({
      guaiMessage,
      messageLength: guaiMessage.length,
      isEmpty: guaiMessage.length === 0,
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
