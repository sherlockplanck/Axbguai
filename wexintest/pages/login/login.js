const ACCOUNT = 'weixingyezuishuai'
const DEFAULT_PASSWORD = 'weixingyezuishuai'
const PASSWORD_KEY = 'smartGuaiPassword'
const LOGIN_STATE_KEY = 'smartGuaiLoggedIn'

Page({
  data: {
    account: '',
    password: '',
    oldPassword: '',
    newPassword: '',
    changingPassword: false,
  },

  onLoad() {
    if (!wx.getStorageSync(PASSWORD_KEY)) {
      wx.setStorageSync(PASSWORD_KEY, DEFAULT_PASSWORD)
    }

    wx.setStorageSync(LOGIN_STATE_KEY, false)
  },

  onAccountInput(e) {
    this.setData({
      account: e.detail.value,
    })
  },

  onPasswordInput(e) {
    this.setData({
      password: e.detail.value,
    })
  },

  onOldPasswordInput(e) {
    this.setData({
      oldPassword: e.detail.value,
    })
  },

  onNewPasswordInput(e) {
    this.setData({
      newPassword: e.detail.value,
    })
  },

  login() {
    const account = this.data.account.trim()
    const password = this.data.password
    const savedPassword = getSavedPassword()

    if (account !== ACCOUNT || password !== savedPassword) {
      wx.showToast({
        title: '账号或密码错误',
        icon: 'none',
      })
      return
    }

    wx.setStorageSync(LOGIN_STATE_KEY, true)
    wx.redirectTo({
      url: '/pages/index/index',
    })
  },

  showChangePassword() {
    this.setData({
      changingPassword: true,
      oldPassword: '',
      newPassword: '',
    })
  },

  hideChangePassword() {
    this.setData({
      changingPassword: false,
      oldPassword: '',
      newPassword: '',
    })
  },

  changePassword() {
    const oldPassword = this.data.oldPassword
    const newPassword = this.data.newPassword

    if (!oldPassword || !newPassword) {
      wx.showToast({
        title: '请填写旧密码和新密码',
        icon: 'none',
      })
      return
    }

    if (oldPassword !== getSavedPassword()) {
      wx.showToast({
        title: '旧密码错误',
        icon: 'none',
      })
      return
    }

    wx.setStorageSync(PASSWORD_KEY, newPassword)
    this.setData({
      changingPassword: false,
      password: '',
      oldPassword: '',
      newPassword: '',
    })
    wx.showToast({
      title: '密码已修改',
      icon: 'success',
    })
  },
})

function getSavedPassword() {
  return wx.getStorageSync(PASSWORD_KEY) || DEFAULT_PASSWORD
}
