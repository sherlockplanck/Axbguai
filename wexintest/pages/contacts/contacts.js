const ONENET = {
  productId: 'C21bW84s72',
  deviceName: 'data',
  token: 'version=2018-10-31&res=products%2FC21bW84s72%2Fdevices%2Fdata&et=1910080768&method=md5&sign=1HjjPj0U%2B0xJoTLkBSn1hw%3D%3D',
  baseUrl: 'https://iot-api.heclouds.com',
}

const PROPERTY = {
  name1: 'name1',
  name2: 'name2',
  contact1: 'contact1',
  contact2: 'contact2',
  contact1Priority: 'contact1_priority',
  contact2Priority: 'contact2_priority',
}

const LOGIN_STATE_KEY = 'smartGuaiLoggedIn'
const CONTACT_STORAGE_KEY = 'smartGuaiContacts'

Page({
  data: {
    name1: '',
    name2: '',
    contact1: '',
    contact2: '',
    contact1Priority: true,
    contact2Priority: false,
    isEditing: false,
    lastUpdate: '未读取',
    loading: false,
    saving: false,
    errorMessage: '',
    submitMessage: '',
  },

  onLoad() {
    if (!isLoggedIn()) {
      redirectToLogin()
      return
    }

    this.restoreSavedContacts()
    this.fetchContacts().catch(() => {})
  },

  onShow() {
    if (!isLoggedIn()) {
      redirectToLogin()
    }
  },

  onPullDownRefresh() {
    this.fetchContacts().then(() => {
      wx.stopPullDownRefresh()
    }).catch(() => {
      wx.stopPullDownRefresh()
    })
  },

  restoreSavedContacts() {
    const savedContacts = getSavedContacts()

    if (!savedContacts) {
      return
    }

    this.setData({
      name1: savedContacts.name1,
      name2: savedContacts.name2,
      contact1: savedContacts.contact1,
      contact2: savedContacts.contact2,
      contact1Priority: savedContacts.contact1Priority,
      contact2Priority: savedContacts.contact2Priority,
      isEditing: false,
    })
  },

  onName1Input(e) {
    this.setData({
      name1: e.detail.value,
    })
  },

  onName2Input(e) {
    this.setData({
      name2: e.detail.value,
    })
  },

  onContact1Input(e) {
    this.setData({
      contact1: e.detail.value,
    })
  },

  onContact2Input(e) {
    this.setData({
      contact2: e.detail.value,
    })
  },

  startEdit() {
    this.setData({
      isEditing: true,
      submitMessage: '',
    })
  },

  selectPriority(e) {
    const contact = e.currentTarget.dataset.contact

    if (!this.data.isEditing) {
      return
    }

    this.setData({
      contact1Priority: contact === '1',
      contact2Priority: contact === '2',
    })
  },

  fetchContacts() {
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
            const message = getResponseMessage(res, '读取联系人失败')
            this.setData({
              errorMessage: message,
            })
            reject(new Error(message))
            return
          }

          const properties = normalizeProperties(res.data && res.data.data)
          const savedContacts = getSavedContacts() || getEmptyContacts()
          const contact1Priority = getPropertyValue(properties, PROPERTY.contact1Priority)
          const contact2Priority = getPropertyValue(properties, PROPERTY.contact2Priority)
          const priority = normalizePriority(
            contact1Priority === '' ? savedContacts.contact1Priority : contact1Priority,
            contact2Priority === '' ? savedContacts.contact2Priority : contact2Priority,
          )
          const contacts = {
            name1: getPropertyValue(properties, PROPERTY.name1) || savedContacts.name1,
            name2: getPropertyValue(properties, PROPERTY.name2) || savedContacts.name2,
            contact1: getPropertyValue(properties, PROPERTY.contact1) || savedContacts.contact1,
            contact2: getPropertyValue(properties, PROPERTY.contact2) || savedContacts.contact2,
            contact1Priority: priority.contact1Priority,
            contact2Priority: priority.contact2Priority,
          }

          saveContactsToStorage(contacts)
          this.setData({
            ...contacts,
            isEditing: false,
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

  saveContacts() {
    if (this.data.saving) {
      return
    }

    const name1 = this.data.name1.trim()
    const name2 = this.data.name2.trim()
    const contact1 = this.data.contact1.trim()
    const contact2 = this.data.contact2.trim()

    if (name1.length > 128 || name2.length > 128 || contact1.length > 128 || contact2.length > 128) {
      wx.showToast({
        title: '姓名和电话不能超过128字符',
        icon: 'none',
      })
      return
    }

    this.setData({
      saving: true,
      submitMessage: '',
    })

    const contact1Priority = this.data.contact1Priority === true
    const contact2Priority = !contact1Priority

    setDeviceProperty(PROPERTY.name1, name1)
      .then(() => setDeviceProperty(PROPERTY.name2, name2))
      .then(() => setDeviceProperty(PROPERTY.contact1, contact1))
      .then(() => setDeviceProperty(PROPERTY.contact2, contact2))
      .then(() => setPriorityProperties(contact1Priority, contact2Priority))
      .then(() => {
        const contacts = {
          name1,
          name2,
          contact1,
          contact2,
          contact1Priority,
          contact2Priority,
        }

        saveContactsToStorage(contacts)
        this.setData({
          ...contacts,
          isEditing: false,
          errorMessage: '',
          submitMessage: '联系人设置已发送到云端',
        })
        wx.showToast({
          title: '设置成功',
          icon: 'success',
        })
      })
      .catch((err) => {
        const message = err.message || err.errMsg || '设置失败'
        this.setData({
          errorMessage: message,
        })
        wx.showToast({
          title: message,
          icon: 'none',
        })
      })
      .then(() => {
        this.setData({
          saving: false,
        })
      })
  },
})


function getSavedContacts() {
  const contacts = wx.getStorageSync(CONTACT_STORAGE_KEY)

  if (!contacts || typeof contacts !== 'object') {
    return null
  }

  const priority = normalizePriority(contacts.contact1Priority, contacts.contact2Priority)

  return {
    ...getEmptyContacts(),
    ...contacts,
    contact1Priority: priority.contact1Priority,
    contact2Priority: priority.contact2Priority,
  }
}

function saveContactsToStorage(contacts) {
  wx.setStorageSync(CONTACT_STORAGE_KEY, {
    ...getEmptyContacts(),
    ...contacts,
  })
}

function getEmptyContacts() {
  return {
    name1: '',
    name2: '',
    contact1: '',
    contact2: '',
    contact1Priority: true,
    contact2Priority: false,
  }
}

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

function normalizePriority(contact1Priority, contact2Priority) {
  if (isTrueValue(contact1Priority)) {
    return {
      contact1Priority: true,
      contact2Priority: false,
    }
  }

  if (isTrueValue(contact2Priority)) {
    return {
      contact1Priority: false,
      contact2Priority: true,
    }
  }

  return {
    contact1Priority: true,
    contact2Priority: false,
  }
}

function isTrueValue(value) {
  const normalized = `${value}`.toLowerCase()
  return normalized === '1' || normalized === 'true'
}

function isSuccessResponse(res) {
  if (res.statusCode < 200 || res.statusCode >= 300) {
    return false
  }

  return !res.data || res.data.code === 0 || res.data.code === 200
}

function setDeviceProperty(identifier, value) {
  return new Promise((resolve, reject) => {
    wx.request({
      url: `${ONENET.baseUrl}/thingmodel/set-device-property`,
      method: 'POST',
      data: {
        product_id: ONENET.productId,
        device_name: ONENET.deviceName,
        params: {
          [identifier]: value,
        },
      },
      header: {
        authorization: ONENET.token,
        'content-type': 'application/json',
      },
      success: (res) => {
        if (!isSuccessResponse(res)) {
          reject(new Error(getResponseMessage(res, `${identifier} 设置失败`)))
          return
        }

        resolve(res)
      },
      fail: (err) => {
        reject(err)
      },
    })
  })
}

function setPriorityProperties(contact1Priority, contact2Priority) {
  const firstProperty = contact1Priority ? PROPERTY.contact2Priority : PROPERTY.contact1Priority
  const secondProperty = contact1Priority ? PROPERTY.contact1Priority : PROPERTY.contact2Priority

  return setDeviceProperty(firstProperty, false)
    .then(() => setDeviceProperty(secondProperty, true))
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
