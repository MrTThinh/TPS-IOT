// Đợi HTML vẽ xong rồi mới lấy .eye, .pupil
window.addEventListener('DOMContentLoaded', () => {
  // Tạo mảng eyes: mỗi phần tử gồm eye, pupil, offsetX
  const eyes = [];
  document.querySelectorAll('.eye').forEach(eye => {
    const pupil = eye.querySelector('.pupil');
    if (!pupil) return;
    eyes.push({
      eye,
      pupil,
      offsetX: 0
    });
  });

  // Hàm tính offset theo kích thước eye & pupil
  const calcOffset = () => {
    for (const props of eyes) {
      // Xóa transform cũ cho chắc
      props.pupil.style.transform = '';

      const eyeRect = props.eye.getBoundingClientRect();
      const pupilRect = props.pupil.getBoundingClientRect();

      // Công thức bạn dùng: căn con ngươi nằm giữa mắt
      props.offsetX =
        (eyeRect.right - pupilRect.right - (pupilRect.left - eyeRect.left)) / 2;
    }
  };

  // Gọi lúc đầu
  calcOffset();

  // Cho mắt nhìn theo chuột
  window.addEventListener('mousemove', (e) => {
    for (const props of eyes) {
      const eyeRect = props.eye.getBoundingClientRect();
      const centerX = (eyeRect.left + eyeRect.right) / 2;
      const centerY = (eyeRect.top + eyeRect.bottom) / 2;

      const dx = e.clientX - centerX;
      const dy = e.clientY - centerY;

      const angle = Math.atan2(dy, dx);

      // khoảng di chuyển tối đa của con ngươi
      const maxMove = props.offsetX || 6;  // nhỏ thôi cho header đỡ loạn
      const moveX = Math.cos(angle) * maxMove;
      const moveY = Math.sin(angle) * maxMove;

      props.pupil.style.transform = `translate(${moveX}px, ${moveY}px)`;
    }
  });

  // Nếu đổi kích thước cửa sổ -> tính lại offset
  window.addEventListener('resize', calcOffset);
});
